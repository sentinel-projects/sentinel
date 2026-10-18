#include "sentinel/core/provider/SelectedEndpointChatProvider.h"

#include "sentinel/core/chat/ChatImageContent.h"
#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelRouting.h"
#include "sentinel/core/provider/protocols/anthropic_messages/AnthropicMessagesProtocol.h"
#include "sentinel/core/provider/protocols/gemini_generate_content/GeminiGenerateContentProtocol.h"
#include "sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/runtime/ProviderRequestRuntime.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>

#include <algorithm>
#include <utility>

namespace sentinel::core {
namespace {
Q_LOGGING_CATEGORY(nativeProviderDiagnostics, "sentinel.provider.diagnostics", QtWarningMsg)

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
                tools.append(provider::anthropicToolDeclaration(
                    provider::openAiCompatibleToolDefinition(tool)));
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
                results.append({call.callId, provider::openAiCompatibleFunctionName(call.toolId),
                                result.content});
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
            if (tool.enabled && tool.exposedToModel &&
                provider::openAiCompatibleFunctionName(tool.id) == name) {
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
        const auto normalizedRequest = provider::buildOpenAiCompatibleNativeRequest(
            binding_.modelId, chatImageContent(message, options.images), options);
        if (!normalizedRequest.error.isEmpty()) {
            return providerFailure(normalizedRequest.error,
                                   ChatProviderErrorCategory::MalformedResponse,
                                   ChatProviderReply::Error::InvalidResponse);
        }
        auto body = normalizedRequest.body;
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
        const auto parsed = provider::normalizeOpenAiCompatibleNativeResponse(completion.body);
        if (!parsed.hasChoices) {
            auto reply =
                providerFailure(QStringLiteral("Chat completion has no choices."),
                                ChatProviderErrorCategory::MalformedResponse,
                                ChatProviderReply::Error::InvalidResponse, completion.httpStatus,
                                completion.attempts, completion.retrySummary);
            reply.requestId = completion.requestId;
            return finish(reply, QStringLiteral("response_format_failure"));
        }
        for (auto it = parsed.diagnostics.begin(); it != parsed.diagnostics.end(); ++it) {
            diagnostics.insert(it.key(), it.value());
        }
        ChatProviderReply result;
        result.httpStatus = completion.httpStatus;
        result.success = true;
        result.lifecycle = ChatRequestLifecycle::Completed;
        result.attempts = completion.attempts;
        result.retrySummary = completion.retrySummary;
        result.requestId = completion.requestId;
        result.message = parsed.text;
        const auto malformed = [&](const QString& detail) {
            auto reply =
                providerFailure(detail, ChatProviderErrorCategory::MalformedResponse,
                                ChatProviderReply::Error::InvalidResponse, completion.httpStatus,
                                completion.attempts, completion.retrySummary);
            reply.requestId = completion.requestId;
            return finish(reply, QStringLiteral("response_format_failure"));
        };
        if (parsed.outputLimit)
            return finish(
                providerFailure(
                    QStringLiteral(
                        "Native provider output budget exhausted (finish_reason=length). "
                        "No tool calls accepted; inspect prior changes before a smaller retry."),
                    ChatProviderErrorCategory::RequestRejected,
                    ChatProviderReply::Error::InvalidResponse, completion.httpStatus,
                    completion.attempts, completion.retrySummary),
                QStringLiteral("output_limit"));
        for (const auto& call : parsed.toolCalls) {
            QString toolId;
            for (const auto& tool : options.tools)
                if (provider::openAiCompatibleFunctionName(tool.id) == call.name) {
                    toolId = tool.id;
                    break;
                }
            const auto arguments =
                provider::parseOpenAiCompatibleJsonObject(call.arguments.toString());
            if (toolId.isEmpty() || call.callId.isEmpty() || !arguments)
                return malformed(QStringLiteral("Invalid or unregistered native tool call."));
            result.toolCalls.append({call.callId, toolId, *arguments});
        }
        if (options.structuredOutput && result.toolCalls.isEmpty()) {
            const auto document = provider::parseOpenAiCompatibleJsonObject(result.message);
            if (!document)
                return malformed(QStringLiteral("Invalid native structured response."));
            result.structuredResult = *document;
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

} // namespace

std::shared_ptr<IChatProvider>
makeSelectedEndpointChatProvider(ModelBinding binding, LMStudioConfig config, int timeoutMs) {
    return std::make_shared<SelectedEndpointChatProvider>(std::move(binding), std::move(config),
                                                          timeoutMs);
}

QJsonObject geminiFunctionDeclaration(const ToolDescriptor& tool) {
    auto function =
        provider::openAiCompatibleToolDefinition(tool).value(QStringLiteral("function")).toObject();
    const auto schema = function.take(QStringLiteral("parameters"));
    function.insert(QStringLiteral("parametersJsonSchema"), schema);
    return function;
}

} // namespace sentinel::core
