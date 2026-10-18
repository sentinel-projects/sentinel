// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/chat/ChatImageContent.h"

#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/chat/OllamaChatProvider.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/model/IModelRouter.h"
#include "sentinel/core/provider/ProviderRegistry.h"
#include "sentinel/core/provider/protocols/anthropic_messages/AnthropicMessagesProtocol.h"
#include "sentinel/core/provider/protocols/gemini_generate_content/GeminiGenerateContentProtocol.h"
#include "sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/runtime/ProviderRequestRuntime.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace sentinel::core {

struct ProviderHealthRegistry {
    struct Entry {
        ProviderHealth health = ProviderHealth::Unknown;
        quint64 sequence = 0;
        QString source;
    };
    std::mutex mutex;
    std::atomic<quint64> nextSequence{0};
    QHash<QString, Entry> entries;
    OllamaModelDiscoveryResult ollamaDiscovery;
    QList<OllamaModelSummary> ollamaModels;
    quint64 ollamaDiscoverySequence = 0;
    struct Catalog {
        QList<OllamaModelSummary> models;
        ProviderDiscoveryOutcome outcome;
        quint64 sequence = 0;
        bool observed = false;
        bool nativeCatalog = false;
    };
    QHash<QString, Catalog> catalogs;
};

QString providerHealthName(ProviderHealth health) {
    switch (health) {
    case ProviderHealth::Unknown:
        return QStringLiteral("Unknown");
    case ProviderHealth::Available:
        return QStringLiteral("Available");
    case ProviderHealth::Degraded:
        return QStringLiteral("Degraded");
    case ProviderHealth::Unavailable:
        return QStringLiteral("Unavailable");
    }
    return QStringLiteral("Unknown");
}

QString providerCatalogStateName(ProviderCatalogState state) {
    switch (state) {
    case ProviderCatalogState::Pending:
        return QStringLiteral("Pending");
    case ProviderCatalogState::Available:
        return QStringLiteral("Available");
    case ProviderCatalogState::Empty:
        return QStringLiteral("Empty catalog");
    case ProviderCatalogState::Failed:
        return QStringLiteral("Discovery failed");
    case ProviderCatalogState::AuthenticationRequired:
        return QStringLiteral("Authentication required");
    case ProviderCatalogState::EndpointUnavailable:
        return QStringLiteral("Endpoint unavailable");
    case ProviderCatalogState::Stale:
        return QStringLiteral("Stale catalog");
    case ProviderCatalogState::ConfiguredModelOnly:
        return QStringLiteral("Configured model only");
    case ProviderCatalogState::Unverified:
        return QStringLiteral("Unverified");
    }
    return QStringLiteral("Unverified");
}

namespace {
Q_LOGGING_CATEGORY(nativeProviderDiagnostics, "sentinel.provider.diagnostics", QtWarningMsg)

class HealthTrackedProvider final : public IChatProvider {
public:
    HealthTrackedProvider(QString id, std::shared_ptr<IChatProvider> delegate,
                          std::shared_ptr<ProviderHealthRegistry> registry)
        : id_(id.trimmed().toLower()), delegate_(std::move(delegate)),
          registry_(std::move(registry)) {}
    QString name() const override {
        return delegate_->name();
    }
    ChatProviderStatus status() const override {
        return delegate_->status();
    }
    ChatProviderConcurrency concurrency() const override {
        return delegate_->concurrency();
    }
    bool supportsStreaming() const override {
        return delegate_->supportsStreaming();
    }
    bool supportsBoundedStreaming() const override {
        return delegate_->supportsBoundedStreaming();
    }
    ChatProviderReply sendMessage(const QString& message) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendMessage(message);
        update(sequence, reply, QStringLiteral("generation"));
        return reply;
    }
    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendRequest(message, options);
        update(sequence, reply, QStringLiteral("generation"));
        return reply;
    }
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendMessageStreaming(message, onDelta, cancellationToken);
        update(sequence, reply, QStringLiteral("stream"));
        return reply;
    }
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken,
                         const ChatRequestOptions& options) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendMessageStreaming(message, onDelta, cancellationToken, options);
        update(sequence, reply, QStringLiteral("stream"));
        return reply;
    }

private:
    void update(quint64 sequence, const ChatProviderReply& reply, const QString& source) {
        std::lock_guard lock(registry_->mutex);
        auto& entry = registry_->entries[id_];
        if (sequence < entry.sequence)
            return;
        entry.sequence = sequence;
        entry.source = source;
        if (reply.success)
            entry.health = ProviderHealth::Available;
        else if (reply.category == ChatProviderErrorCategory::RateLimited ||
                 reply.category == ChatProviderErrorCategory::ConnectionFailed ||
                 reply.category == ChatProviderErrorCategory::Timeout ||
                 reply.category == ChatProviderErrorCategory::MalformedResponse)
            entry.health = ProviderHealth::Degraded;
        else if (reply.category == ChatProviderErrorCategory::ProviderUnavailable ||
                 reply.category == ChatProviderErrorCategory::AuthenticationRequired)
            entry.health = ProviderHealth::Unavailable;
    }
    QString id_;
    std::shared_ptr<IChatProvider> delegate_;
    std::shared_ptr<ProviderHealthRegistry> registry_;
};

QString nativeFunctionName(const QString& toolId) {
    static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9_-]{1,64}$"));
    if (valid.match(toolId).hasMatch())
        return toolId;
    return QStringLiteral("tool_%1").arg(QString::fromLatin1(
        QCryptographicHash::hash(toolId.toUtf8(), QCryptographicHash::Sha256).toHex().left(48)));
}

QJsonObject nativeToolDefinition(const ToolDescriptor& tool) {
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
             QJsonObject{{QStringLiteral("name"), nativeFunctionName(tool.id)},
                         {QStringLiteral("description"), tool.description.left(200)},
                         {QStringLiteral("parameters"), schema}}}};
}

ChatProviderErrorCategory categoryFromLocalInference(LocalInferenceError error,
                                                     LocalInferenceStatus status) {
    if (error == LocalInferenceError::Timeout)
        return ChatProviderErrorCategory::Timeout;
    if (error == LocalInferenceError::MissingModel ||
        error == LocalInferenceError::ModelUnavailable ||
        status == LocalInferenceStatus::ModelUnavailable)
        return ChatProviderErrorCategory::ModelNotFound;
    if (error == LocalInferenceError::PermissionDenied)
        return ChatProviderErrorCategory::AuthenticationRequired;
    if (error == LocalInferenceError::InvalidResponse)
        return ChatProviderErrorCategory::MalformedResponse;
    if (error == LocalInferenceError::EndpointUnreachable ||
        error == LocalInferenceError::OllamaNotRunning)
        return ChatProviderErrorCategory::ConnectionFailed;
    if (error == LocalInferenceError::BlankPrompt ||
        error == LocalInferenceError::EndpointBlocked ||
        error == LocalInferenceError::SafetyBlocked)
        return ChatProviderErrorCategory::RequestRejected;
    return ChatProviderErrorCategory::ProviderUnavailable;
}

ChatRequestLifecycle lifecycleForCategory(ChatProviderErrorCategory category) {
    if (category == ChatProviderErrorCategory::Timeout)
        return ChatRequestLifecycle::TimedOut;
    if (category == ChatProviderErrorCategory::RateLimited)
        return ChatRequestLifecycle::RateLimited;
    if (category == ChatProviderErrorCategory::Cancelled)
        return ChatRequestLifecycle::Cancelled;
    return ChatRequestLifecycle::Failed;
}

ChatProviderReply
providerFailure(QString detail, ChatProviderErrorCategory category,
                ChatProviderReply::Error error = ChatProviderReply::Error::ProviderFailure,
                int httpStatus = 0, int attempts = 1, QString retrySummary = {}) {
    ChatProviderReply reply;
    reply.success = false;
    reply.errorMessage = std::move(detail);
    reply.error = error;
    reply.category = category;
    reply.lifecycle = lifecycleForCategory(category);
    reply.httpStatus = httpStatus;
    reply.attempts = qMax(1, attempts);
    reply.retrySummary = std::move(retrySummary);
    return reply;
}

ChatProviderReply nativeEndpointReply(LMStudioLocalInferenceClient::NativeProtocol protocol,
                                      const ModelBinding& binding, const LMStudioConfig& config,
                                      int timeoutMs, const QString& message,
                                      const ChatRequestOptions& options) {
    const bool claude = protocol == LMStudioLocalInferenceClient::NativeProtocol::Claude;
    const auto malformed = [](const QString& detail) {
        return providerFailure(detail, ChatProviderErrorCategory::MalformedResponse,
                               ChatProviderReply::Error::InvalidResponse);
    };
    if (options.structuredOutput && options.structuredSchema.isEmpty())
        return malformed(QStringLiteral("Native structured schema is missing."));
    if (options.priorToolCalls.size() != options.toolResults.size())
        return malformed(QStringLiteral("Native tool results are incomplete."));
    for (const auto& call : options.priorToolCalls)
        if (std::none_of(options.tools.cbegin(), options.tools.cend(),
                         [&](const ToolDescriptor& tool) {
                             return tool.enabled && tool.exposedToModel && tool.id == call.toolId;
                         }))
            return malformed(QStringLiteral("Native tool continuation is not registered."));

    QJsonObject body;
    if (claude) {
        QJsonArray messages{QJsonObject{
            {QStringLiteral("role"), QStringLiteral("user")},
            {QStringLiteral("content"), chatImageContent(message, options.images, true)}}};
        if (!options.priorToolCalls.isEmpty()) {
            QJsonParseError historyError;
            const auto history = QJsonDocument::fromJson(
                options.priorToolCalls.first().providerContinuation.toUtf8(), &historyError);
            if (historyError.error != QJsonParseError::NoError || !history.isArray() ||
                history.array().isEmpty())
                return malformed(QStringLiteral("Claude tool continuation history is missing."));
            for (const auto& call : options.priorToolCalls)
                if (call.providerContinuation !=
                    options.priorToolCalls.first().providerContinuation)
                    return malformed(QStringLiteral("Claude tool continuation histories differ."));
            messages = history.array();
            QList<provider::AnthropicToolResult> results;
            for (int i = 0; i < options.priorToolCalls.size(); ++i) {
                const auto& call = options.priorToolCalls.at(i);
                const auto& result = options.toolResults.at(i);
                if (call.callId.isEmpty() || call.callId != result.callId)
                    return malformed(QStringLiteral("Native tool result ID mismatch."));
                results.append({call.callId, result.content});
            }
            messages.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("content"), provider::anthropicToolResultBlocks(results)}});
        }
        QJsonArray tools;
        if (options.nativeToolCalling) {
            for (const auto& tool : options.tools) {
                if (!tool.enabled || !tool.exposedToModel)
                    continue;
                tools.append(provider::anthropicToolDeclaration(nativeToolDefinition(tool)));
            }
        }
        body = provider::buildAnthropicNativeRequestBody(
            binding.modelId, messages, options.maxOutputTokens, tools,
            options.structuredOutput ? options.structuredSchema : QJsonObject{});
    } else {
        QJsonArray initialParts{QJsonObject{{QStringLiteral("text"), message}}};
        appendGeminiImageParts(initialParts, options.images);
        QJsonArray contents{QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                                        {QStringLiteral("parts"), initialParts}}};
        if (!options.priorToolCalls.isEmpty()) {
            QJsonParseError historyError;
            const auto history = QJsonDocument::fromJson(
                options.priorToolCalls.first().providerContinuation.toUtf8(), &historyError);
            if (historyError.error != QJsonParseError::NoError || !history.isArray() ||
                history.array().isEmpty())
                return malformed(
                    QStringLiteral("Gemini function continuation history is missing."));
            for (const auto& call : options.priorToolCalls)
                if (call.providerContinuation !=
                    options.priorToolCalls.first().providerContinuation)
                    return malformed(
                        QStringLiteral("Gemini function continuation histories differ."));
            contents = history.array();
            QList<provider::GeminiFunctionResult> results;
            for (int i = 0; i < options.priorToolCalls.size(); ++i) {
                const auto& call = options.priorToolCalls.at(i);
                const auto& result = options.toolResults.at(i);
                if (call.callId.isEmpty() || call.callId != result.callId)
                    return malformed(QStringLiteral("Native function result ID mismatch."));
                results.append({call.callId, nativeFunctionName(call.toolId), result.content});
            }
            contents.append(QJsonObject{
                {QStringLiteral("role"), QStringLiteral("user")},
                {QStringLiteral("parts"), provider::geminiFunctionResponseParts(results)}});
        }
        QJsonArray declarations;
        if (options.nativeToolCalling) {
            for (const auto& tool : options.tools) {
                if (!tool.enabled || !tool.exposedToModel)
                    continue;
                declarations.append(geminiFunctionDeclaration(tool));
            }
        }
        body = provider::buildGeminiNativeRequestBody(
            binding.modelId, contents, options.maxOutputTokens, declarations,
            options.structuredOutput ? options.structuredSchema : QJsonObject{});
    }

    LMStudioLocalInferenceClient client(config, timeoutMs);
    const auto completion = client.completeNativeChat(protocol, body, options.cancellationToken);
    if (!completion.ok) {
        // Precedence: cancellation, an explicit native capability rejection, then the
        // transport-classified status/category. The Qt UnknownContentError flag is a
        // last-resort fallback so it cannot override a validated HTTP status such as
        // 408 (Timeout) or 429 (RateLimited).
        const auto classified =
            completion.cancelled ? ChatProviderErrorCategory::Cancelled
            : completion.httpStatus == 400 || completion.httpStatus == 422 ||
                    completion.httpStatus == 501
                ? ChatProviderErrorCategory::CapabilityUnsupported
            : completion.providerErrorCategory > 0
                ? static_cast<ChatProviderErrorCategory>(completion.providerErrorCategory)
            : completion.malformed
                ? ChatProviderErrorCategory::MalformedResponse
                : ProviderRequestRuntime::classify(completion.httpStatus, completion.networkError,
                                                   completion.timedOut, completion.cancelled);
        const auto category = classified == ChatProviderErrorCategory::None
                                  ? ChatProviderErrorCategory::ProviderUnavailable
                                  : classified;
        auto reply =
            providerFailure(completion.error, category,
                            category == ChatProviderErrorCategory::CapabilityUnsupported
                                ? ChatProviderReply::Error::CapabilityRejected
                                : ChatProviderReply::Error::ProviderFailure,
                            completion.httpStatus, completion.attempts, completion.retrySummary);
        reply.requestId = completion.requestId;
        return reply;
    }
    ChatProviderReply result;
    result.success = true;
    result.lifecycle = ChatRequestLifecycle::Completed;
    result.httpStatus = completion.httpStatus;
    result.attempts = completion.attempts;
    result.retrySummary = completion.retrySummary;
    result.requestId = completion.requestId;
    const auto invalid = [&](const QString& detail) {
        auto reply = malformed(detail);
        reply.httpStatus = completion.httpStatus;
        reply.attempts = completion.attempts;
        reply.retrySummary = completion.retrySummary;
        reply.requestId = completion.requestId;
        return reply;
    };
    QString claudeContinuation;
    QString geminiContinuation;
    // Native tool-name resolution, continuation wiring and the gemini-3
    // signature policy stay in this adapter; the protocol modules only normalize
    // the response into text plus {callId, name, arguments, signature} parts.
    const auto appendToolCall = [&](const QString& callId, const QString& name,
                                    const QJsonValue& arguments, const QString& signature,
                                    const QString& continuation) -> QString {
        QString toolId;
        for (const auto& tool : options.tools)
            if (tool.enabled && tool.exposedToModel && nativeFunctionName(tool.id) == name) {
                toolId = tool.id;
                break;
            }
        if (!options.nativeToolCalling || toolId.isEmpty() || callId.isEmpty() ||
            !arguments.isObject())
            return QStringLiteral("Invalid or unregistered native function call.");
        if (!claude && binding.modelId.startsWith(QLatin1String("gemini-3")) &&
            result.toolCalls.isEmpty() && signature.isEmpty())
            return QStringLiteral("Gemini function response lacks a continuation signature.");
        result.toolCalls.append({callId, toolId, arguments.toObject(), continuation});
        return {};
    };
    if (claude) {
        const auto parsed = provider::parseAnthropicMessagesResponse(completion.body);
        if (!parsed.hasContent)
            return invalid(QStringLiteral("Native provider response has no content."));
        result.message = parsed.text;
        auto history = body.value(QStringLiteral("messages")).toArray();
        history.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                   {QStringLiteral("content"), parsed.contentBlocks}});
        claudeContinuation =
            QString::fromUtf8(QJsonDocument(history).toJson(QJsonDocument::Compact));
        for (const auto& call : parsed.toolUses) {
            const auto error =
                appendToolCall(call.callId, call.name, call.input, QString{}, claudeContinuation);
            if (!error.isEmpty())
                return invalid(error);
        }
    } else {
        const auto parsed =
            provider::parseGeminiGenerateContentResponse(completion.body, completion.requestId);
        if (parsed.status == provider::GeminiGenerateContentResponseStatus::Blocked) {
            auto reply =
                providerFailure(QStringLiteral("Gemini blocked the request."),
                                ChatProviderErrorCategory::RequestRejected,
                                ChatProviderReply::Error::ProviderFailure, completion.httpStatus,
                                completion.attempts, completion.retrySummary);
            reply.requestId = completion.requestId;
            return reply;
        }
        if (parsed.status == provider::GeminiGenerateContentResponseStatus::Rejected) {
            auto reply =
                providerFailure(QStringLiteral("Gemini rejected the response."),
                                ChatProviderErrorCategory::RequestRejected,
                                ChatProviderReply::Error::ProviderFailure, completion.httpStatus,
                                completion.attempts, completion.retrySummary);
            reply.requestId = completion.requestId;
            return reply;
        }
        if (parsed.status != provider::GeminiGenerateContentResponseStatus::Ok)
            return invalid(QStringLiteral("Native provider response has no content."));
        result.message = parsed.text;
        auto history = body.value(QStringLiteral("contents")).toArray();
        history.append(parsed.modelContent);
        geminiContinuation =
            QString::fromUtf8(QJsonDocument(history).toJson(QJsonDocument::Compact));
        for (const auto& call : parsed.functionCalls) {
            const auto error = appendToolCall(call.callId, call.name, call.args,
                                              call.thoughtSignature, geminiContinuation);
            if (!error.isEmpty())
                return invalid(error);
        }
    }
    if (options.structuredOutput && result.toolCalls.isEmpty()) {
        QJsonParseError parseError;
        const auto document = QJsonDocument::fromJson(result.message.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            return invalid(QStringLiteral("Native structured response is malformed."));
        result.structuredResult = document.object();
    }
    if (result.message.isEmpty() && result.toolCalls.isEmpty())
        return invalid(QStringLiteral("Native provider response has no text or function calls."));
    return result;
}

// IChatProvider implementation for every non-Ollama provider id. The binding
// fixes the model for the whole request/run; configuration comes from the
// persisted endpoint and credential settings resolved by ModelService.
class SelectedEndpointChatProvider final : public IChatProvider {
public:
    SelectedEndpointChatProvider(ModelBinding binding, LMStudioConfig config, int timeoutMs)
        : binding_(std::move(binding)), config_(std::move(config)), timeoutMs_(timeoutMs) {}

    QString name() const override {
        return binding_.providerId;
    }
    ChatProviderStatus status() const override {
        return config_.isAllowedEndpoint() ? ChatProviderStatus::Ready
                                           : ChatProviderStatus::Unavailable;
    }
    ChatProviderConcurrency concurrency() const override {
        return ChatProviderConcurrency::Supported;
    }
    bool supportsStreaming() const override {
        return binding_.capabilities.streaming == CapabilitySupport::Supported;
    }
    bool supportsBoundedStreaming() const override {
        return binding_.capabilities.streaming == CapabilitySupport::Supported;
    }
    ChatProviderReply sendMessage(const QString& message) override {
        return sendMessageWithToken(message, {});
    }
    ChatProviderReply sendMessageWithToken(
        const QString& message, const std::shared_ptr<std::atomic_bool>& cancellationToken,
        const QList<ChatImage>& images = {}, int deadlineMs = 0, int maxOutputTokens = 0) {
        if (!config_.isAllowedEndpoint())
            return providerFailure(
                QStringLiteral("Selected provider '%1' is unavailable or not configured.")
                    .arg(binding_.providerId),
                ChatProviderErrorCategory::ProviderUnavailable);
        LocalInferenceRequest request;
        request.prompt = message;
        request.images = images;
        request.options.model = binding_.modelId;
        request.options.timeoutMs = deadlineMs > 0 ? deadlineMs : timeoutMs_;
        if (maxOutputTokens > 0)
            request.options.maxTokens = maxOutputTokens;
        request.options.cancellationToken = cancellationToken;
        LMStudioLocalInferenceClient client(config_, timeoutMs_);
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = client.infer(request);
        const auto finish = [&](ChatProviderReply reply) {
            reply.diagnostics = result.diagnostics;
            reply.diagnostics.insert(QStringLiteral("provider_id"), binding_.providerId.left(128));
            reply.diagnostics.insert(QStringLiteral("model_id"), binding_.modelId.left(128));
            reply.diagnostics.insert(QStringLiteral("deadline_ms"), request.options.timeoutMs);
            reply.diagnostics.insert(QStringLiteral("output_budget_tokens"),
                                     request.options.maxTokens);
            reply.diagnostics.insert(QStringLiteral("elapsed_ms"), elapsed.elapsed());
            reply.diagnostics.insert(QStringLiteral("http_status"), result.httpStatus);
            reply.diagnostics.insert(QStringLiteral("category"),
                                     chatProviderErrorCategoryName(reply.category));
            reply.diagnostics.insert(QStringLiteral("upstream_cancellation"),
                                     QStringLiteral("unconfirmed"));
            if (binding_.capabilities.contextWindow)
                reply.diagnostics.insert(QStringLiteral("reported_context_tokens"),
                                         *binding_.capabilities.contextWindow);
            qCInfo(nativeProviderDiagnostics).noquote()
                << QJsonDocument(reply.diagnostics).toJson(QJsonDocument::Compact);
            return reply;
        };
        if (result.status == LocalInferenceStatus::Succeeded) {
            ChatProviderReply reply{true, result.text, {}};
            reply.lifecycle = ChatRequestLifecycle::Completed;
            reply.attempts = result.attempts;
            reply.retrySummary = result.retrySummary;
            reply.requestId = result.requestId;
            reply.httpStatus = result.httpStatus;
            return finish(reply);
        }
        if (result.error == LocalInferenceError::ModelUnavailable)
            return providerFailure(
                QStringLiteral("Selected model '%1' is unavailable from provider '%2'.")
                    .arg(binding_.modelId, binding_.providerId),
                ChatProviderErrorCategory::ModelNotFound);
        auto reply = providerFailure(
            QStringLiteral("Provider '%1': %2").arg(binding_.providerId, result.summary),
            result.providerErrorCategory > 0
                ? static_cast<ChatProviderErrorCategory>(result.providerErrorCategory)
                : categoryFromLocalInference(result.error, result.status),
            ChatProviderReply::Error::ProviderFailure, result.httpStatus, result.attempts,
            result.retrySummary);
        reply.requestId = result.requestId;
        return finish(reply);
    }

    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        if (!options.images.isEmpty() &&
            binding_.capabilities.visionInput == CapabilitySupport::Unsupported)
            return providerFailure(QStringLiteral("Selected model does not support image input."),
                                   ChatProviderErrorCategory::CapabilityUnsupported,
                                   ChatProviderReply::Error::CapabilityRejected);
        if ((options.nativeToolCalling &&
             binding_.capabilities.nativeToolCalling != CapabilitySupport::Supported) ||
            (options.structuredOutput &&
             binding_.capabilities.structuredOutput != CapabilitySupport::Supported) ||
            (options.nativeToolCalling && options.structuredOutput &&
             binding_.capabilities.combinedToolsAndStructuredOutput !=
                 CapabilitySupport::Supported))
            return providerFailure(
                QStringLiteral("Selected model does not support the requested native capability."),
                ChatProviderErrorCategory::CapabilityUnsupported,
                ChatProviderReply::Error::CapabilityRejected);
        if (!options.nativeToolCalling && !options.structuredOutput)
            return sendMessageWithToken(message, options.cancellationToken, options.images,
                                        options.deadlineMs, options.maxOutputTokens);
        if (!config_.isAllowedEndpoint())
            return providerFailure(QStringLiteral("Selected endpoint is unavailable."),
                                   ChatProviderErrorCategory::ProviderUnavailable);
        const auto host = config_.endpoint.host().toLower();
        if (host == QLatin1String("api.anthropic.com"))
            return nativeEndpointReply(
                LMStudioLocalInferenceClient::NativeProtocol::Claude, binding_, config_,
                options.deadlineMs > 0 ? options.deadlineMs : timeoutMs_, message, options);
        if (host == QLatin1String("generativelanguage.googleapis.com"))
            return nativeEndpointReply(
                LMStudioLocalInferenceClient::NativeProtocol::Gemini, binding_, config_,
                options.deadlineMs > 0 ? options.deadlineMs : timeoutMs_, message, options);
        QJsonArray messages{
            QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                        {QStringLiteral("content"), chatImageContent(message, options.images)}}};
        if (!options.priorToolCalls.isEmpty()) {
            if (options.priorToolCalls.size() != options.toolResults.size())
                return providerFailure(QStringLiteral("Native tool results are incomplete."),
                                       ChatProviderErrorCategory::MalformedResponse,
                                       ChatProviderReply::Error::InvalidResponse);
            QJsonArray calls;
            for (const auto& call : options.priorToolCalls) {
                calls.append(QJsonObject{
                    {QStringLiteral("id"), call.callId},
                    {QStringLiteral("type"), QStringLiteral("function")},
                    {QStringLiteral("function"),
                     QJsonObject{{QStringLiteral("name"), nativeFunctionName(call.toolId)},
                                 {QStringLiteral("arguments"),
                                  QString::fromUtf8(QJsonDocument(call.arguments)
                                                        .toJson(QJsonDocument::Compact))}}}});
            }
            messages.append(QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                                        {QStringLiteral("tool_calls"), calls}});
            for (int i = 0; i < options.toolResults.size(); ++i) {
                const auto& result = options.toolResults.at(i);
                if (result.callId != options.priorToolCalls.at(i).callId)
                    return providerFailure(QStringLiteral("Native tool result IDs do not match."),
                                           ChatProviderErrorCategory::MalformedResponse,
                                           ChatProviderReply::Error::InvalidResponse);
                messages.append(
                    QJsonObject{{QStringLiteral("role"), QStringLiteral("tool")},
                                {QStringLiteral("tool_call_id"), result.callId},
                                {QStringLiteral("content"), result.content.left(6800)}});
            }
        }
        QJsonObject body{{QStringLiteral("model"), binding_.modelId},
                         {QStringLiteral("messages"), messages},
                         {QStringLiteral("stream"), false}};
        if (options.nativeToolCalling) {
            QJsonArray tools;
            for (const auto& tool : options.tools)
                if (tool.enabled && tool.exposedToModel)
                    tools.append(nativeToolDefinition(tool));
            if (!tools.isEmpty()) {
                body.insert(QStringLiteral("tools"), tools);
                body.insert(QStringLiteral("tool_choice"), QStringLiteral("auto"));
            }
        }
        if (options.structuredOutput) {
            if (options.structuredSchemaName.isEmpty() || options.structuredSchema.isEmpty())
                return providerFailure(QStringLiteral("Structured planner schema is missing."),
                                       ChatProviderErrorCategory::MalformedResponse,
                                       ChatProviderReply::Error::InvalidResponse);
            body.insert(
                QStringLiteral("response_format"),
                QJsonObject{{QStringLiteral("type"), QStringLiteral("json_schema")},
                            {QStringLiteral("json_schema"),
                             QJsonObject{{QStringLiteral("name"), options.structuredSchemaName},
                                         {QStringLiteral("strict"), options.strictStructuredOutput},
                                         {QStringLiteral("schema"), options.structuredSchema}}}});
        }
        // Budget the actual serialized native request, including schemas and tool
        // results omitted by ContextEngine's prompt estimate. This is an estimate,
        // not a tokenizer measurement; the provider may still reject the request.
        const int requestedBudget = options.maxOutputTokens > 0 ? options.maxOutputTokens : 1024;
        const int outputBudget =
            qMin(requestedBudget, binding_.capabilities.maxOutputTokens.value_or(requestedBudget));
        body.insert(QStringLiteral("max_tokens"), qMax(1, outputBudget));
        const auto bytes = QJsonDocument(body).toJson(QJsonDocument::Compact).size();
        const auto estimatedInput = (bytes + 2) / 3 + 256;
        QJsonObject diagnostics{
            {QStringLiteral("provider_id"), binding_.providerId.left(128)},
            {QStringLiteral("model_id"), binding_.modelId.left(128)},
            {QStringLiteral("request_bytes"), bytes},
            {QStringLiteral("estimated_input_tokens"), estimatedInput},
            {QStringLiteral("estimate_method"), QStringLiteral("utf8_bytes/3+256")},
            {QStringLiteral("requested_output_tokens"), qMax(1, outputBudget)},
            {QStringLiteral("continuation_calls"), options.priorToolCalls.size()},
            {QStringLiteral("transport_attempted"), false}};
        if (binding_.capabilities.contextWindow)
            diagnostics.insert(QStringLiteral("reported_context_tokens"),
                               *binding_.capabilities.contextWindow);
        QJsonArray observationSizes;
        for (const auto& observation : options.toolResults.mid(0, 8))
            observationSizes.append(observation.content.toUtf8().size());
        diagnostics.insert(QStringLiteral("tool_observation_bytes"), observationSizes);
        QElapsedTimer requestElapsed;
        requestElapsed.start();
        diagnostics.insert(QStringLiteral("entered_utc"),
                           QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        diagnostics.insert(QStringLiteral("deadline_ms"),
                           options.deadlineMs > 0 ? options.deadlineMs : timeoutMs_);
        QString diagnosticRequestId;
        const auto finish = [&](ChatProviderReply reply, const QString& outcome) {
            if (reply.requestId.isEmpty())
                reply.requestId = diagnosticRequestId;
            auto metadata = diagnostics;
            metadata.insert(QStringLiteral("outcome"), outcome);
            metadata.insert(QStringLiteral("request_id"), reply.requestId.left(128));
            metadata.insert(QStringLiteral("exited_utc"),
                            QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            metadata.insert(QStringLiteral("elapsed_ms"), requestElapsed.elapsed());
            metadata.insert(QStringLiteral("cancelled"),
                            options.cancellationToken && options.cancellationToken->load());
            metadata.insert(QStringLiteral("http_status"), reply.httpStatus);
            metadata.insert(QStringLiteral("attempts"), reply.attempts);
            metadata.insert(QStringLiteral("error_category"),
                            chatProviderErrorCategoryName(reply.category));
            reply.diagnostics = metadata;
            qCInfo(nativeProviderDiagnostics).noquote()
                << QJsonDocument(metadata).toJson(QJsonDocument::Compact);
            return reply;
        };
        if (binding_.capabilities.contextWindow &&
            estimatedInput + outputBudget > *binding_.capabilities.contextWindow)
            return finish(
                providerFailure(
                    QStringLiteral("Context budget exceeded (estimated input %1, output %2, "
                                   "reported window %3). "
                                   "No request sent; inspect partial changes and reduce task size.")
                        .arg(estimatedInput)
                        .arg(outputBudget)
                        .arg(*binding_.capabilities.contextWindow),
                    ChatProviderErrorCategory::RequestRejected),
                QStringLiteral("context_exhausted"));
        diagnostics.insert(QStringLiteral("transport_attempted"), true);
        LMStudioLocalInferenceClient client(config_, options.deadlineMs > 0 ? options.deadlineMs
                                                                            : timeoutMs_);
        const auto completion = client.completeOpenAiChat(body, options.cancellationToken);
        diagnosticRequestId = completion.requestId;
        if (!completion.ok) {
            const bool contextRejected =
                (completion.httpStatus == 400 || completion.httpStatus == 422) &&
                (completion.error.contains(QStringLiteral("context size has been exceeded"),
                                           Qt::CaseInsensitive) ||
                 completion.error.contains(QStringLiteral("context length exceeded"),
                                           Qt::CaseInsensitive) ||
                 completion.error.contains(QStringLiteral("maximum context length"),
                                           Qt::CaseInsensitive));
            const bool rejected = (options.nativeToolCalling || options.structuredOutput) &&
                                  (completion.httpStatus == 400 || completion.httpStatus == 422 ||
                                   completion.httpStatus == 501);
            // Precedence: cancellation, explicit context/capability rejection, then the
            // transport-classified status/category. The Qt UnknownContentError flag is a
            // last-resort fallback so a validated HTTP status such as 408 (Timeout) or
            // 429 (RateLimited) is never collapsed into MalformedResponse.
            auto reply = providerFailure(
                completion.error,
                completion.cancelled ? ChatProviderErrorCategory::Cancelled
                : contextRejected    ? ChatProviderErrorCategory::RequestRejected
                : rejected           ? ChatProviderErrorCategory::CapabilityUnsupported
                : completion.providerErrorCategory > 0
                    ? static_cast<ChatProviderErrorCategory>(completion.providerErrorCategory)
                : completion.malformed ? ChatProviderErrorCategory::MalformedResponse
                                       : ProviderRequestRuntime::classify(
                                             completion.httpStatus, completion.networkError,
                                             completion.timedOut, completion.cancelled),
                rejected && !contextRejected ? ChatProviderReply::Error::CapabilityRejected
                                             : ChatProviderReply::Error::ProviderFailure,
                completion.httpStatus, completion.attempts, completion.retrySummary);
            reply.requestId = completion.requestId;
            return finish(reply, contextRejected ? QStringLiteral("context_exhausted")
                                 : rejected      ? QStringLiteral("provider_request_rejected")
                                 : completion.cancelled ? QStringLiteral("cancelled")
                                 : completion.timedOut  ? QStringLiteral("timeout")
                                 : reply.category == ChatProviderErrorCategory::MalformedResponse
                                     ? QStringLiteral("response_format_failure")
                                     : QStringLiteral("transport_failure"));
        }
        const auto choices = completion.body.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            auto reply =
                providerFailure(QStringLiteral("Chat completion has no choices."),
                                ChatProviderErrorCategory::MalformedResponse,
                                ChatProviderReply::Error::InvalidResponse, completion.httpStatus,
                                completion.attempts, completion.retrySummary);
            reply.requestId = completion.requestId;
            return finish(reply, QStringLiteral("response_format_failure"));
        }
        const auto replyMessage =
            choices.first().toObject().value(QStringLiteral("message")).toObject();
        const auto rawFinish = choices.first().toObject().value(QStringLiteral("finish_reason"));
        if (rawFinish.isString()) {
            const auto reason = rawFinish.toString();
            diagnostics.insert(QStringLiteral("finish_reason"),
                               QStringList{QStringLiteral("stop"), QStringLiteral("length"),
                                           QStringLiteral("tool_calls"),
                                           QStringLiteral("content_filter")}
                                       .contains(reason)
                                   ? reason
                                   : QStringLiteral("other"));
        }
        const auto usage = completion.body.value(QStringLiteral("usage")).toObject();
        for (const auto& key : {"prompt_tokens", "completion_tokens", "total_tokens"}) {
            const auto value = usage.value(QLatin1String(key));
            if (value.isDouble() && value.toDouble() >= 0)
                diagnostics.insert(QString::fromLatin1(key), value);
        }
        const auto reasoningTokens = usage.value(QStringLiteral("completion_tokens_details"))
                                         .toObject()
                                         .value(QStringLiteral("reasoning_tokens"));
        if (reasoningTokens.isDouble() && reasoningTokens.toDouble() >= 0)
            diagnostics.insert(QStringLiteral("reasoning_tokens"), reasoningTokens);
        diagnostics.insert(
            QStringLiteral("response_content_bytes"),
            replyMessage.value(QStringLiteral("content")).toString().toUtf8().size());
        diagnostics.insert(QStringLiteral("response_tool_calls"),
                           replyMessage.value(QStringLiteral("tool_calls")).toArray().size());
        ChatProviderReply result;
        result.httpStatus = completion.httpStatus;
        result.success = true;
        result.lifecycle = ChatRequestLifecycle::Completed;
        result.attempts = completion.attempts;
        result.retrySummary = completion.retrySummary;
        result.requestId = completion.requestId;
        result.message = replyMessage.value(QStringLiteral("content")).toString();
        const auto malformed = [&](const QString& detail) {
            auto reply =
                providerFailure(detail, ChatProviderErrorCategory::MalformedResponse,
                                ChatProviderReply::Error::InvalidResponse, completion.httpStatus,
                                completion.attempts, completion.retrySummary);
            reply.requestId = completion.requestId;
            return finish(reply, QStringLiteral("response_format_failure"));
        };
        if (rawFinish.toString() == QLatin1String("length"))
            return finish(
                providerFailure(
                    QStringLiteral(
                        "Native provider output budget exhausted (finish_reason=length). "
                        "No tool calls accepted; inspect prior changes before a smaller retry."),
                    ChatProviderErrorCategory::RequestRejected,
                    ChatProviderReply::Error::InvalidResponse, completion.httpStatus,
                    completion.attempts, completion.retrySummary),
                QStringLiteral("output_limit"));
        for (const auto& call : provider::parseOpenAiCompatibleToolCalls(completion.body)) {
            QString toolId;
            for (const auto& tool : options.tools)
                if (nativeFunctionName(tool.id) == call.name) {
                    toolId = tool.id;
                    break;
                }
            QJsonParseError parseError;
            const auto arguments =
                QJsonDocument::fromJson(call.arguments.toString().toUtf8(), &parseError);
            if (toolId.isEmpty() || call.callId.isEmpty() || !arguments.isObject() ||
                parseError.error != QJsonParseError::NoError)
                return malformed(QStringLiteral("Invalid or unregistered native tool call."));
            result.toolCalls.append({call.callId, toolId, arguments.object()});
        }
        if (options.structuredOutput && result.toolCalls.isEmpty()) {
            QJsonParseError parseError;
            const auto document = QJsonDocument::fromJson(result.message.toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !document.isObject())
                return malformed(QStringLiteral("Invalid native structured response."));
            result.structuredResult = document.object();
        }
        if (result.message.isEmpty() && result.toolCalls.isEmpty())
            return finish(providerFailure(
                              QStringLiteral("Chat completion returned no content or tool calls."),
                              ChatProviderErrorCategory::MalformedResponse,
                              ChatProviderReply::Error::InvalidResponse, completion.httpStatus,
                              completion.attempts, completion.retrySummary),
                          QStringLiteral("empty_response"));
        return finish(result, result.toolCalls.isEmpty() ? QStringLiteral("content")
                                                         : QStringLiteral("tool_calls"));
    }

    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken) override {
        if (binding_.capabilities.streaming != CapabilitySupport::Supported)
            return providerFailure(QStringLiteral("Selected model does not support streaming."),
                                   ChatProviderErrorCategory::CapabilityUnsupported,
                                   ChatProviderReply::Error::CapabilityRejected);
        return streamSelected(message, onDelta, cancellationToken, {}, timeoutMs_, 0, false);
    }
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken,
                         const ChatRequestOptions& options) override {
        if (binding_.capabilities.streaming != CapabilitySupport::Supported) {
            return providerFailure(QStringLiteral("Selected model does not support streaming."),
                                   ChatProviderErrorCategory::CapabilityUnsupported,
                                   ChatProviderReply::Error::CapabilityRejected);
        }
        if (!options.images.isEmpty() &&
            binding_.capabilities.visionInput == CapabilitySupport::Unsupported)
            return providerFailure(QStringLiteral("Selected model does not support image input."),
                                   ChatProviderErrorCategory::CapabilityUnsupported,
                                   ChatProviderReply::Error::CapabilityRejected);
        if (options.structuredOutput || options.nativeToolCalling) {
            return providerFailure(
                QStringLiteral(
                    "Bounded streaming does not carry structured or native tool output."),
                ChatProviderErrorCategory::CapabilityUnsupported,
                ChatProviderReply::Error::CapabilityRejected);
        }
        if (options.requireBoundedStreaming && options.deadlineMs <= 0) {
            return providerFailure(
                QStringLiteral("Bounded streaming requires a positive request deadline."),
                ChatProviderErrorCategory::RequestRejected,
                ChatProviderReply::Error::InvalidResponse);
        }
        const auto token = cancellationToken ? cancellationToken : options.cancellationToken;
        const int deadlineMs = options.deadlineMs > 0 ? options.deadlineMs : timeoutMs_;
        return streamSelected(message, onDelta, token, options.images, deadlineMs,
                              options.maxOutputTokens, options.requireBoundedStreaming);
    }

private:
    ChatProviderReply streamSelected(const QString& message,
                                     const std::function<void(const QString&)>& onDelta,
                                     const std::shared_ptr<std::atomic_bool>& cancellationToken,
                                     const QList<ChatImage>& images, int deadlineMs,
                                     int maxOutputTokens, bool requireBoundedStreaming) {
        LocalInferenceRequest request;
        request.prompt = message;
        request.images = images;
        request.options.model = binding_.modelId;
        request.options.timeoutMs = deadlineMs;
        request.options.requireBoundedStreaming = requireBoundedStreaming;
        if (maxOutputTokens > 0) {
            request.options.maxTokens = maxOutputTokens;
        }
        request.options.cancellationToken = cancellationToken;
        LMStudioLocalInferenceStreamClient client(config_, timeoutMs_);
        const auto result =
            client.startStream(request, [&](const LocalInferenceStreamChunk& chunk) {
                if (!chunk.text.isEmpty() && onDelta)
                    onDelta(chunk.text);
            });
        if (result.status == LocalInferenceStreamStatus::Completed) {
            ChatProviderReply reply{true, result.accumulatedText, {}};
            reply.lifecycle = ChatRequestLifecycle::Completed;
            reply.requestId = result.requestId;
            reply.diagnostics.insert(QStringLiteral("deadline_ms"), result.timeoutMs);
            reply.diagnostics.insert(QStringLiteral("bounded_streaming"), requireBoundedStreaming);
            return reply;
        }
        auto reply = providerFailure(
            result.summary,
            result.providerErrorCategory > 0
                ? static_cast<ChatProviderErrorCategory>(result.providerErrorCategory)
            : result.cancelled
                ? ChatProviderErrorCategory::Cancelled
                : categoryFromLocalInference(result.error, LocalInferenceStatus::Error),
            ChatProviderReply::Error::ProviderFailure, result.httpStatus, result.attempts,
            result.retrySummary);
        reply.lifecycle = result.lifecycle;
        reply.requestId = result.requestId;
        reply.diagnostics.insert(QStringLiteral("deadline_ms"), result.timeoutMs);
        reply.diagnostics.insert(QStringLiteral("bounded_streaming"), requireBoundedStreaming);
        return reply;
    }

    ModelBinding binding_;
    LMStudioConfig config_;
    int timeoutMs_ = 0;
};

QString normalizedProviderId(const QString& providerId) {
    return providerId.trimmed().toLower();
}

bool isCloudProviderId(const QString& providerId) {
    const auto* descriptor = ProviderRegistry::builtins().find(providerId);
    return descriptor && descriptor->kind == ProviderKind::Cloud;
}

QString defaultProviderId() {
    return QStringLiteral("ollama");
}

ModelCapabilities mergedCapabilities(ModelCapabilities base, const ModelCapabilities& override,
                                     ModelMetadataSource source) {
    auto merge = [source](CapabilitySupport& target, ModelMetadataSource& provenance,
                          CapabilitySupport value, ModelMetadataSource suppliedSource) {
        if (value != CapabilitySupport::Unknown) {
            target = value;
            provenance = suppliedSource == ModelMetadataSource::Unknown ? source : suppliedSource;
        }
    };
    merge(base.streaming, base.provenance.streaming, override.streaming,
          override.provenance.streaming);
    merge(base.structuredOutput, base.provenance.structuredOutput, override.structuredOutput,
          override.provenance.structuredOutput);
    merge(base.nativeToolCalling, base.provenance.nativeToolCalling, override.nativeToolCalling,
          override.provenance.nativeToolCalling);
    merge(base.combinedToolsAndStructuredOutput, base.provenance.combinedToolsAndStructuredOutput,
          override.combinedToolsAndStructuredOutput,
          override.provenance.combinedToolsAndStructuredOutput);
    merge(base.visionInput, base.provenance.visionInput, override.visionInput,
          override.provenance.visionInput);
    merge(base.audioInput, base.provenance.audioInput, override.audioInput,
          override.provenance.audioInput);
    merge(base.audioOutput, base.provenance.audioOutput, override.audioOutput,
          override.provenance.audioOutput);
    if (override.contextWindow && *override.contextWindow > 0) {
        base.contextWindow = override.contextWindow;
        base.provenance.contextWindow =
            override.provenance.contextWindow == ModelMetadataSource::Unknown
                ? source
                : override.provenance.contextWindow;
    }
    if (override.maxOutputTokens && *override.maxOutputTokens > 0) {
        base.maxOutputTokens = override.maxOutputTokens;
        base.provenance.maxOutputTokens =
            override.provenance.maxOutputTokens == ModelMetadataSource::Unknown
                ? source
                : override.provenance.maxOutputTokens;
    }
    return base;
}

} // namespace

QJsonObject geminiFunctionDeclaration(const ToolDescriptor& tool) {
    auto function = nativeToolDefinition(tool).value(QStringLiteral("function")).toObject();
    const auto schema = function.take(QStringLiteral("parameters"));
    function.insert(QStringLiteral("parametersJsonSchema"), schema);
    return function;
}

QString modelBindingErrorName(ModelBindingError error) {
    switch (error) {
    case ModelBindingError::None:
        return QStringLiteral("None");
    case ModelBindingError::ProviderNotFound:
        return QStringLiteral("ProviderNotFound");
    case ModelBindingError::ProviderUnavailable:
        return QStringLiteral("ProviderUnavailable");
    case ModelBindingError::ModelNotFound:
        return QStringLiteral("ModelNotFound");
    case ModelBindingError::ConfigurationInvalid:
        return QStringLiteral("ConfigurationInvalid");
    case ModelBindingError::AuthenticationRequired:
        return QStringLiteral("AuthenticationRequired");
    }

    return QStringLiteral("None");
}

QString modelBindingFailureSummary(const ModelBindingResolution& resolution) {
    const auto providerId = resolution.binding.providerId.isEmpty() ? QStringLiteral("<none>")
                                                                    : resolution.binding.providerId;
    const auto modelId = resolution.binding.modelId.isEmpty() ? QStringLiteral("<none>")
                                                              : resolution.binding.modelId;
    return QStringLiteral("Model binding failed for provider '%1' / model '%2': %3 (%4)")
        .arg(providerId, modelId, resolution.reason, modelBindingErrorName(resolution.error));
}

ModelService::ModelService(AppSettings* settings, QObject* parent)
    : QObject(parent), settings_(settings),
      ollamaEndpoint_(OllamaEndpoint::defaultEndpoint().toString()),
      lmStudioEndpoint_(QStringLiteral("http://127.0.0.1:1234")),
      llamaCppEndpoint_(QStringLiteral("http://127.0.0.1:8080")) {
    providerHealthRegistry_ = std::make_shared<ProviderHealthRegistry>();
    providerHealthRegistry_->ollamaDiscovery.lifecycle = ChatRequestLifecycle::Pending;
    providerHealthRegistry_->ollamaDiscovery.errorCategory = ChatProviderErrorCategory::None;
    providerHealthRegistry_->ollamaDiscovery.safeDetail =
        QStringLiteral("Ollama model discovery pending.");
    if (settings_) {
        selection_.providerId = normalizedProviderId(settings_->selectedRuntimeProvider());
        selection_.modelId = settings_->selectedModelForProvider(selection_.providerId).trimmed();
        connect(settings_, &AppSettings::cloudApiKeysChanged, this, [this] {
            {
                std::lock_guard lock(providerHealthRegistry_->mutex);
                for (const auto& descriptor : ProviderRegistry::builtins().descriptors()) {
                    if (descriptor.kind == ProviderKind::Cloud) {
                        providerHealthRegistry_->catalogs.remove(descriptor.id);
                    }
                }
            }
            emit providerRegistryChanged();
        });
        connect(settings_, &AppSettings::selectedCloudProviderChanged, this, [this] {
            {
                std::lock_guard lock(providerHealthRegistry_->mutex);
                providerHealthRegistry_->catalogs.remove(QStringLiteral("cloud-api"));
            }
            emit providerRegistryChanged();
            emit modelCapabilitiesChanged();
        });
    } else {
        selection_.providerId = defaultProviderId();
    }

    for (const auto& descriptor : ProviderRegistry::builtins().descriptors()) {
        if (descriptor.adapter == ProviderAdapter::Ollama) {
            registerProvider(
                descriptor.id,
                [this](const ModelBinding& binding) {
                    auto provider = std::make_shared<OllamaChatProvider>(
                        OllamaConfig::fromEndpoint(ollamaEndpoint_), localInferenceTimeoutMs_);
                    provider->setSelectedModel(binding.modelId);
                    provider->setDiscoverySnapshot(ollamaDiscovery());
                    return std::shared_ptr<IChatProvider>(std::move(provider));
                },
                descriptor.declaredCapabilities);
        } else {
            registerProvider(
                descriptor.id,
                [this](const ModelBinding& binding) {
                    return std::make_shared<SelectedEndpointChatProvider>(
                        binding, providerConfig(binding), localInferenceTimeoutMs_);
                },
                descriptor.declaredCapabilities);
        }
    }
}

ModelService::~ModelService() = default;

void ModelService::setModelRouter(IModelRouter* router) {
    router_ = router;
}

void ModelService::registerProvider(const QString& providerId, ModelProviderFactory factory,
                                    ModelCapabilities defaults) {
    const auto normalized = normalizedProviderId(providerId);
    if (normalized.isEmpty() || !factory)
        return;
    providerFactories_.insert(normalized, std::move(factory));
    providerCapabilities_.insert(normalized, std::move(defaults));
    emit providerRegistryChanged();
}

void ModelService::setModelCapabilities(const QString& providerId, const QString& modelId,
                                        ModelCapabilities capabilities) {
    const auto provider = normalizedProviderId(providerId);
    const auto model = modelId.trimmed();
    if (!provider.isEmpty() && !model.isEmpty()) {
        modelCapabilities_[provider].insert(model, std::move(capabilities));
        emit modelCapabilitiesChanged();
    }
}

ModelCapabilities ModelService::capabilityOverrides(const QString& providerId,
                                                    const QString& modelId) const {
    return settings_ ? settings_->modelCapabilitiesOverride(providerId, modelId)
                     : ModelCapabilities{};
}

void ModelService::setCapabilityOverrides(const QString& providerId, const QString& modelId,
                                          const ModelCapabilities& overrides) {
    if (!settings_ || providerId.trimmed().isEmpty() || modelId.trimmed().isEmpty())
        return;
    settings_->setModelCapabilitiesOverride(providerId, modelId, overrides);
    emit modelCapabilitiesChanged();
}

void ModelService::clearCapabilityOverrides(const QString& providerId, const QString& modelId) {
    setCapabilityOverrides(providerId, modelId, {});
}

ModelCapabilities ModelService::capabilities(const QString& providerId,
                                             const QString& modelId) const {
    const auto provider = normalizedProviderId(providerId);
    const auto model = modelId.trimmed();
    auto result = mergedCapabilities({}, providerCapabilities_.value(provider),
                                     ModelMetadataSource::ProviderDefault);
    if (provider == QLatin1String("cloud-api")) {
        const auto host = providerConfig(ModelBinding{provider, model}).endpoint.host().toLower();
        if (host == QLatin1String("api.anthropic.com") ||
            host == QLatin1String("generativelanguage.googleapis.com")) {
            result.nativeToolCalling = CapabilitySupport::Supported;
            result.provenance.nativeToolCalling = ModelMetadataSource::ProviderDefault;
            result.structuredOutput = CapabilitySupport::Unknown;
            result.provenance.structuredOutput = ModelMetadataSource::Unknown;
        }
    }
    if (router_) {
        const auto route = router_->resolveSelection(ModelBinding{provider, model});
        if (route.status == ModelRoutingStatus::Routed) {
            if (route.provider.id == provider)
                result = mergedCapabilities(result, route.provider.modelCapabilities,
                                            ModelMetadataSource::ProviderOwnedMetadata);
            if (route.model.providerId == provider && route.model.id == model) {
                result = mergedCapabilities(result, route.model.capabilities,
                                            ModelMetadataSource::ProviderOwnedMetadata);
                if (route.model.contextWindowTokens > 0 && !result.contextWindow) {
                    result.contextWindow = route.model.contextWindowTokens;
                    result.provenance.contextWindow = ModelMetadataSource::ProviderOwnedMetadata;
                }
            }
        }
    }
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        const auto& models = provider == QLatin1String("ollama")
                                 ? providerHealthRegistry_->ollamaModels
                                 : providerHealthRegistry_->catalogs.value(provider).models;
        for (const auto& discovered : models) {
            if (discovered.name == model) {
                result = mergedCapabilities(
                    result, discovered.capabilities,
                    provider == QLatin1String("ollama") || provider == QLatin1String("lm-studio") ||
                            provider == QLatin1String("llama-cpp-server") ||
                            provider == QLatin1String("openai-compatible-local")
                        ? ModelMetadataSource::RuntimeReported
                        : ModelMetadataSource::ProviderCatalog);
                break;
            }
        }
    }
    result = mergedCapabilities(result, modelCapabilities_.value(provider).value(model),
                                ModelMetadataSource::ProgrammaticMetadata);
    if (settings_)
        result = mergedCapabilities(result, settings_->modelCapabilitiesOverride(provider, model),
                                    ModelMetadataSource::UserOverride);
    return result;
}

CurrentModelMetadata ModelService::currentModelMetadata(const QString& providerId,
                                                        const QString& modelId) const {
    CurrentModelMetadata metadata;
    metadata.providerId = normalizedProviderId(providerId);
    metadata.modelId = modelId.trimmed();
    metadata.providerKind =
        isCloudProviderId(metadata.providerId) ? ProviderKind::Cloud : ProviderKind::Local;
    metadata.catalog = providerStatus(metadata.providerId, metadata.modelId).catalog;
    metadata.capabilities = capabilities(metadata.providerId, metadata.modelId);
    const auto models = providerDiscoveredModels(metadata.providerId);
    for (const auto& discovered : models) {
        if (discovered.name == metadata.modelId) {
            metadata.family = discovered.family;
            metadata.publisher = discovered.publisher;
            metadata.architecture = discovered.architecture;
            break;
        }
    }
    return metadata;
}

bool ModelService::isKnownProvider(const QString& providerId) const {
    return providerFactories_.contains(normalizedProviderId(providerId));
}

QStringList ModelService::knownProviderIds() const {
    auto ids = providerFactories_.keys();
    ids.sort();
    return ids;
}

ModelSelection ModelService::selectedModel() const {
    return selection_;
}

void ModelService::setSelectedModel(const ModelSelection& selection) {
    setSelectedProviderId(selection.providerId);
    setSelectedModelId(selection.modelId);
}

void ModelService::setSelectedProviderId(const QString& providerId) {
    const auto normalized = normalizedProviderId(providerId);
    const auto selected =
        providerFactories_.contains(normalized) ? normalized : defaultProviderId();
    if (selected == selection_.providerId)
        return;
    selection_.providerId = selected;
    if (settings_)
        settings_->setSelectedRuntimeProvider(selected);
    emit selectedModelChanged();
}

void ModelService::setSelectedModelId(const QString& modelId) {
    const auto normalized = modelId.trimmed();
    if (normalized == selection_.modelId)
        return;
    selection_.modelId = normalized;
    if (settings_ && !selection_.providerId.isEmpty())
        settings_->setSelectedModelForProvider(selection_.providerId, normalized);
    emit selectedModelChanged();
}

ModelBindingResolution ModelService::resolve(const ModelSelection& selection) {
    return resolve(selection.providerId, selection.modelId);
}

ModelBindingResolution ModelService::resolve(const QString& providerId, const QString& modelId) {
    ModelBindingResolution resolution;
    const auto provider = normalizedProviderId(providerId);
    const auto model = modelId.trimmed();
    resolution.binding = ModelBinding{provider, model};
    resolution.selection = ModelSelection{provider, model};

    if (provider.isEmpty() || !providerFactories_.contains(provider)) {
        resolution.error = ModelBindingError::ProviderNotFound;
        resolution.reason = provider.isEmpty()
                                ? QStringLiteral("No provider is configured for this request.")
                                : QStringLiteral("Provider '%1' is not configured.").arg(provider);
        return resolution;
    }
    if (provider == QLatin1String("ollama")) {
        const auto discovery = ollamaDiscovery();
        if (discovery.lifecycle != ChatRequestLifecycle::Pending &&
            discovery.lifecycle != ChatRequestLifecycle::Running && !discovery.succeeded()) {
            resolution.error = ModelBindingError::ProviderUnavailable;
            resolution.reason = discovery.safeDetail;
            return resolution;
        }
        if (discovery.succeeded() && discovery.models.isEmpty()) {
            resolution.error = ModelBindingError::ModelNotFound;
            resolution.reason = discovery.safeDetail;
            return resolution;
        }
        if (discovery.succeeded() && !model.isEmpty()) {
            const bool available =
                std::any_of(discovery.models.cbegin(), discovery.models.cend(),
                            [&](const OllamaModelSummary& item) { return item.name == model; });
            if (!available) {
                resolution.error = ModelBindingError::ModelNotFound;
                resolution.reason = QStringLiteral("Selected Ollama model is unavailable.");
                return resolution;
            }
        }
    }
    if (model.isEmpty()) {
        resolution.error = ModelBindingError::ModelNotFound;
        resolution.reason = QStringLiteral("Provider '%1' has no configured model.").arg(provider);
        return resolution;
    }
    if (router_) {
        const auto route = router_->resolveSelection(resolution.binding);
        if (route.status != ModelRoutingStatus::Routed) {
            resolution.error = ModelBindingError::ProviderUnavailable;
            resolution.reason =
                QStringLiteral("Provider '%1' has no available model route.").arg(provider);
            return resolution;
        }
    }
    resolution.binding.capabilities = capabilities(provider, model);
    resolution.binding.contextWindowTokens =
        resolution.binding.capabilities.contextWindow.value_or(0);
    if (provider != QLatin1String("ollama")) {
        const auto config = providerConfig(resolution.binding);
        if (isCloudProviderId(provider) && config.apiKey.trimmed().isEmpty()) {
            resolution.error = ModelBindingError::AuthenticationRequired;
            resolution.reason =
                QStringLiteral("Provider '%1' requires an API key that is not configured.")
                    .arg(provider);
            return resolution;
        }
        if (!config.isAllowedEndpoint()) {
            resolution.error = ModelBindingError::ConfigurationInvalid;
            resolution.reason =
                QStringLiteral("Provider '%1' is not configured with a usable endpoint.")
                    .arg(provider);
            return resolution;
        }
        const auto catalog = providerStatus(provider, model);
        if (catalog.catalog == ProviderCatalogState::AuthenticationRequired) {
            resolution.error = ModelBindingError::AuthenticationRequired;
            resolution.reason = catalog.safeDetail;
            return resolution;
        }
        if (catalog.catalog == ProviderCatalogState::Failed ||
            catalog.catalog == ProviderCatalogState::EndpointUnavailable) {
            resolution.error = ModelBindingError::ProviderUnavailable;
            resolution.reason = catalog.safeDetail;
            return resolution;
        }
        if (catalog.catalog == ProviderCatalogState::Empty ||
            (catalog.catalog == ProviderCatalogState::Available &&
             !catalog.modelIds.contains(model))) {
            resolution.error = ModelBindingError::ModelNotFound;
            resolution.reason =
                catalog.catalog == ProviderCatalogState::Empty
                    ? catalog.safeDetail
                    : QStringLiteral("Selected model is unavailable in the provider catalog.");
            return resolution;
        }
    }

    resolution.provider = constructProvider(resolution.binding);
    if (!resolution.provider) {
        resolution.error = ModelBindingError::ProviderUnavailable;
        resolution.reason = QStringLiteral("Provider '%1' could not be constructed.").arg(provider);
    }
    return resolution;
}

std::shared_ptr<IChatProvider> ModelService::constructProvider(const ModelBinding& binding) const {
    const auto factory = providerFactories_.value(binding.providerId);
    auto provider = factory ? factory(binding) : nullptr;
    return provider ? std::make_shared<HealthTrackedProvider>(
                          binding.providerId, std::move(provider), providerHealthRegistry_)
                    : nullptr;
}

ProviderHealth ModelService::providerHealth(const QString& providerId) const {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    return providerHealthRegistry_->entries.value(normalizedProviderId(providerId)).health;
}

ProviderCompletenessStatus ModelService::providerStatus(const QString& providerId,
                                                        const QString& modelId) const {
    ProviderCompletenessStatus status;
    status.providerId = normalizedProviderId(providerId);
    status.health = providerHealth(status.providerId);
    const auto selected = modelId.trimmed().isEmpty() && selection_.providerId == status.providerId
                              ? selection_.modelId
                              : modelId.trimmed();
    status.capabilities = capabilities(status.providerId, selected);
    if (!isKnownProvider(status.providerId)) {
        status.catalog = ProviderCatalogState::Failed;
        status.errorCategory = ChatProviderErrorCategory::ProviderUnavailable;
        status.safeDetail = QStringLiteral("Provider is not registered.");
        return status;
    }
    if (status.providerId == QLatin1String("ollama")) {
        const auto discovery = ollamaDiscovery();
        status.errorCategory = discovery.errorCategory;
        status.safeDetail = discovery.safeDetail;
        if (discovery.succeeded()) {
            for (const auto& model : discoveredOllamaModels())
                status.modelIds.append(model.name);
            status.catalog = status.modelIds.isEmpty() ? ProviderCatalogState::Empty
                                                       : ProviderCatalogState::Available;
        } else if (discovery.lifecycle == ChatRequestLifecycle::Pending ||
                   discovery.lifecycle == ChatRequestLifecycle::Running) {
            status.catalog = ProviderCatalogState::Pending;
        } else {
            for (const auto& model : discoveredOllamaModels())
                status.modelIds.append(model.name);
            status.catalog =
                !status.modelIds.isEmpty() ? ProviderCatalogState::Stale
                : discovery.errorCategory == ChatProviderErrorCategory::AuthenticationRequired
                    ? ProviderCatalogState::AuthenticationRequired
                : discovery.errorCategory == ChatProviderErrorCategory::ConnectionFailed
                    ? ProviderCatalogState::EndpointUnavailable
                    : ProviderCatalogState::Failed;
        }
        return status;
    }
    const ModelBinding binding{status.providerId, selected};
    const auto config = providerConfig(binding);
    if (config.isCloud() && config.apiKey.trimmed().isEmpty()) {
        status.catalog = ProviderCatalogState::AuthenticationRequired;
        status.errorCategory = ChatProviderErrorCategory::AuthenticationRequired;
        status.safeDetail = QStringLiteral("Provider API key is not configured.");
    } else if (!config.isAllowedEndpoint()) {
        status.catalog = ProviderCatalogState::EndpointUnavailable;
        status.errorCategory = ChatProviderErrorCategory::ProviderUnavailable;
        status.safeDetail = QStringLiteral("Provider endpoint is not configured or allowed.");
    } else {
        ProviderHealthRegistry::Catalog catalog;
        {
            std::lock_guard lock(providerHealthRegistry_->mutex);
            catalog = providerHealthRegistry_->catalogs.value(status.providerId);
        }
        if (catalog.observed) {
            status.errorCategory = catalog.outcome.category;
            for (const auto& model : catalog.models)
                status.modelIds.append(model.name);
            if (catalog.outcome.completed) {
                status.catalog = status.modelIds.isEmpty() ? ProviderCatalogState::Empty
                                                           : ProviderCatalogState::Available;
                status.safeDetail =
                    status.modelIds.isEmpty()
                        ? QStringLiteral("Provider returned an empty model catalog.")
                        : QStringLiteral("Provider model catalog is available.");
            } else {
                if (config.isCloud() && catalog.models.isEmpty() &&
                    catalog.outcome.httpStatus == 404) {
                    status.catalog = ProviderCatalogState::ConfiguredModelOnly;
                    if (status.modelIds.isEmpty() && !selected.isEmpty())
                        status.modelIds.append(selected);
                    status.safeDetail =
                        QStringLiteral("Provider model-list endpoint is unavailable; configured "
                                       "model is unverified.");
                    return status;
                }
                status.catalog =
                    !status.modelIds.isEmpty() ? ProviderCatalogState::Stale
                    : catalog.outcome.category == ChatProviderErrorCategory::AuthenticationRequired
                        ? ProviderCatalogState::AuthenticationRequired
                    : catalog.outcome.category == ChatProviderErrorCategory::ConnectionFailed
                        ? ProviderCatalogState::EndpointUnavailable
                        : ProviderCatalogState::Failed;
                status.safeDetail =
                    QStringLiteral("Provider model discovery failed (%1).")
                        .arg(chatProviderErrorCategoryName(catalog.outcome.category));
            }
        } else if (config.isCloud()) {
            status.catalog = ProviderCatalogState::ConfiguredModelOnly;
            if (!selected.isEmpty())
                status.modelIds.append(selected);
            status.safeDetail =
                QStringLiteral("Configured model; live provider catalog is unavailable.");
        } else {
            status.catalog = ProviderCatalogState::Unverified;
            status.safeDetail = QStringLiteral("Local endpoint catalog has not been observed yet.");
        }
    }
    return status;
}

ProviderCompletenessStatus ModelService::providerStatusSnapshot(const QString& providerId,
                                                                const QString& modelId) const {
    const auto id = normalizedProviderId(providerId);
    if (!isCloudProviderId(id))
        return providerStatus(id, modelId);
    ProviderCompletenessStatus status;
    status.providerId = id;
    status.health = providerHealth(id);
    status.capabilities = mergedCapabilities({}, providerCapabilities_.value(id),
                                             ModelMetadataSource::ProviderDefault);
    ProviderHealthRegistry::Catalog catalog;
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        catalog = providerHealthRegistry_->catalogs.value(id);
    }
    if (!catalog.observed) {
        status.safeDetail =
            QStringLiteral("Catalog unverified; credentials are not inspected by terminal status. "
                           "Select this provider and refresh discovery explicitly.");
        return status;
    }
    status.errorCategory = catalog.outcome.category;
    for (const auto& model : catalog.models)
        status.modelIds.append(model.name);
    if (catalog.outcome.completed) {
        status.catalog = status.modelIds.isEmpty() ? ProviderCatalogState::Empty
                                                   : ProviderCatalogState::Available;
        status.safeDetail = QStringLiteral("Observed provider model catalog.");
    } else {
        status.catalog =
            !status.modelIds.isEmpty() ? ProviderCatalogState::Stale
            : catalog.outcome.category == ChatProviderErrorCategory::AuthenticationRequired
                ? ProviderCatalogState::AuthenticationRequired
                : ProviderCatalogState::Failed;
        status.safeDetail = QStringLiteral("Observed discovery failure (%1).")
                                .arg(chatProviderErrorCategoryName(catalog.outcome.category));
    }
    return status;
}

QList<OllamaModelSummary> ModelService::providerDiscoveredModels(const QString& providerId) const {
    const auto id = normalizedProviderId(providerId);
    std::lock_guard lock(providerHealthRegistry_->mutex);
    if (id == QLatin1String("ollama"))
        return providerHealthRegistry_->ollamaModels;
    return providerHealthRegistry_->catalogs.value(id).models;
}

void ModelService::acceptProviderDiscovery(const QString& providerId,
                                           const QList<OllamaModelSummary>& models,
                                           const ProviderDiscoveryOutcome& outcome,
                                           quint64 sequence, bool nativeCatalog) {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    auto& catalog = providerHealthRegistry_->catalogs[normalizedProviderId(providerId)];
    if (sequence < catalog.sequence)
        return;
    if (catalog.nativeCatalog && !nativeCatalog) {
        catalog.sequence = sequence;
        return;
    }
    catalog.sequence = sequence;
    catalog.observed = true;
    catalog.outcome = outcome;
    if (outcome.completed && (!catalog.nativeCatalog || nativeCatalog)) {
        catalog.models = models;
        catalog.nativeCatalog = nativeCatalog;
    }
}

void ModelService::acceptOllamaDiscovery(const OllamaModelDiscoveryResult& result,
                                         quint64 sequence) {
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        if (sequence < providerHealthRegistry_->ollamaDiscoverySequence)
            return;
        providerHealthRegistry_->ollamaDiscoverySequence = sequence;
        providerHealthRegistry_->ollamaDiscovery = result;
        if (result.succeeded())
            providerHealthRegistry_->ollamaModels = result.models;
    }
    reportProviderDiscovery(QStringLiteral("ollama"), sequence, result.succeeded(),
                            result.errorCategory);
}

OllamaModelDiscoveryResult ModelService::ollamaDiscovery() const {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    return providerHealthRegistry_->ollamaDiscovery;
}

QList<OllamaModelSummary> ModelService::discoveredOllamaModels() const {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    return providerHealthRegistry_->ollamaModels;
}

void ModelService::applyOllamaModelMutation(const QString& modelId, bool installed) {
    const auto name = modelId.trimmed();
    if (name.isEmpty())
        return;
    std::lock_guard lock(providerHealthRegistry_->mutex);
    auto& models = providerHealthRegistry_->ollamaModels;
    models.removeIf([&](const auto& model) { return model.name == name; });
    if (providerHealthRegistry_->ollamaDiscovery.succeeded()) {
        providerHealthRegistry_->ollamaDiscovery.models.removeIf(
            [&](const auto& model) { return model.name == name; });
    }
    if (installed) {
        OllamaModelSummary model;
        model.name = name;
        models.append(model);
        if (providerHealthRegistry_->ollamaDiscovery.succeeded())
            providerHealthRegistry_->ollamaDiscovery.models.append(model);
    }
}

quint64 ModelService::beginProviderHealthObservation() const {
    return ++providerHealthRegistry_->nextSequence;
}

void ModelService::reportProviderDiscovery(const QString& providerId, quint64 sequence,
                                           bool completed, ChatProviderErrorCategory category) {
    reportProviderRequest(providerId, sequence, completed, category, QStringLiteral("discovery"));
}

void ModelService::reportProviderRequest(const QString& providerId, quint64 sequence,
                                         bool completed, ChatProviderErrorCategory category,
                                         const QString& source) {
    bool changed = false;
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        auto& entry = providerHealthRegistry_->entries[normalizedProviderId(providerId)];
        if (sequence < entry.sequence)
            return;
        const auto previous = entry.health;
        entry.sequence = sequence;
        entry.source = source;
        if (completed)
            entry.health = ProviderHealth::Available;
        else if (category == ChatProviderErrorCategory::RateLimited ||
                 category == ChatProviderErrorCategory::ConnectionFailed ||
                 category == ChatProviderErrorCategory::Timeout ||
                 category == ChatProviderErrorCategory::ProviderUnavailable)
            entry.health = ProviderHealth::Degraded;
        else if (category == ChatProviderErrorCategory::AuthenticationRequired ||
                 category == ChatProviderErrorCategory::RequestRejected ||
                 category == ChatProviderErrorCategory::MalformedResponse)
            entry.health = ProviderHealth::Unavailable;
        changed = previous != entry.health;
    }
    if (changed)
        emit providerHealthChanged();
}

LMStudioConfig ModelService::providerConfig(const ModelBinding& binding) const {
    AppSettings* liveSettings = settings_;
    std::unique_ptr<AppSettings> fallbackSettings;
    if (!liveSettings) {
        const QString settingsPath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                .filePath(QStringLiteral("settings.json"));
        fallbackSettings =
            std::make_unique<AppSettings>(std::make_unique<JsonSettingsStore>(settingsPath));
        liveSettings = fallbackSettings.get();
    }

    LMStudioConfig config;
    const QString cloudProv = liveSettings->selectedCloudProvider().toLower().trimmed();

    const auto* descriptor =
        ProviderRegistry::builtins().endpointDescriptor(binding.providerId, cloudProv);
    config.endpoint = descriptor && !descriptor->baseEndpoint.isEmpty()
                          ? QUrl(descriptor->baseEndpoint)
                          : QUrl(lmStudioEndpoint_);
    if (descriptor) {
        switch (descriptor->endpoint) {
        case ProviderEndpoint::Anthropic:
            config.apiKey = liveSettings->claudeApiKey();
            break;
        case ProviderEndpoint::Gemini:
            config.apiKey = liveSettings->geminiApiKey();
            break;
        case ProviderEndpoint::DeepSeek:
            config.apiKey = liveSettings->deepseekApiKey();
            break;
        case ProviderEndpoint::Groq:
            config.apiKey = liveSettings->groqApiKey();
            break;
        case ProviderEndpoint::Mistral:
            config.apiKey = liveSettings->mistralApiKey();
            break;
        case ProviderEndpoint::OpenAi:
            config.apiKey = liveSettings->openAiApiKey();
            break;
        case ProviderEndpoint::LlamaCpp:
            config.endpoint = QUrl(llamaCppEndpoint_);
            break;
        default:
            break;
        }
    }
    config.networkFactory = providerNetworkFactory_;
    return config;
}

void ModelService::setOllamaEndpoint(const QString& endpoint) {
    if (ollamaEndpoint_ != endpoint) {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        providerHealthRegistry_->ollamaModels.clear();
        providerHealthRegistry_->ollamaDiscovery = {};
        providerHealthRegistry_->ollamaDiscovery.lifecycle = ChatRequestLifecycle::Pending;
        providerHealthRegistry_->ollamaDiscovery.errorCategory = ChatProviderErrorCategory::None;
        providerHealthRegistry_->ollamaDiscovery.safeDetail =
            QStringLiteral("Ollama model discovery pending.");
        providerHealthRegistry_->entries.remove(QStringLiteral("ollama"));
    }
    ollamaEndpoint_ = endpoint;
}

void ModelService::setLmStudioEndpoint(const QString& endpoint) {
    if (lmStudioEndpoint_ != endpoint) {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        providerHealthRegistry_->catalogs.remove(QStringLiteral("lm-studio"));
    }
    lmStudioEndpoint_ = endpoint;
}

void ModelService::setLlamaCppEndpoint(const QString& endpoint) {
    if (llamaCppEndpoint_ != endpoint) {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        providerHealthRegistry_->catalogs.remove(QStringLiteral("llama-cpp-server"));
    }
    llamaCppEndpoint_ = endpoint;
}

void ModelService::setLocalInferenceTimeoutMs(int timeoutMs) {
    localInferenceTimeoutMs_ = timeoutMs;
}

void ModelService::setProviderNetworkFactoryForTesting(
    std::shared_ptr<ILocalInferenceNetworkFactory> networkFactory) {
    providerNetworkFactory_ = std::move(networkFactory);
}

} // namespace sentinel::core
