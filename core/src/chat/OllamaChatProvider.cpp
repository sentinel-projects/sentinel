// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/chat/OllamaChatProvider.h"

#include "sentinel/core/runtime/LocalInference.h"

#include <QUuid>

#include <algorithm>
#include <utility>

namespace sentinel::core {
namespace {

ChatProviderErrorCategory categoryFromLocalInference(LocalInferenceError error,
                                                     LocalInferenceStatus status) {
    if (error == LocalInferenceError::Timeout)
        return ChatProviderErrorCategory::Timeout;
    if (error == LocalInferenceError::OllamaNotRunning ||
        error == LocalInferenceError::ClientUnavailable)
        return ChatProviderErrorCategory::ProviderUnavailable;
    if (error == LocalInferenceError::EndpointUnreachable)
        return ChatProviderErrorCategory::ConnectionFailed;
    if (error == LocalInferenceError::MissingModel ||
        error == LocalInferenceError::ModelUnavailable ||
        status == LocalInferenceStatus::ModelUnavailable)
        return ChatProviderErrorCategory::ModelNotFound;
    if (error == LocalInferenceError::InvalidResponse ||
        error == LocalInferenceError::StreamInterrupted)
        return ChatProviderErrorCategory::MalformedResponse;
    if (error == LocalInferenceError::BlankPrompt)
        return ChatProviderErrorCategory::RequestRejected;
    return ChatProviderErrorCategory::ProviderUnavailable;
}

ChatProviderReply
failureReply(QString detail, ChatProviderErrorCategory category,
             ChatProviderReply::Error error = ChatProviderReply::Error::ProviderFailure) {
    ChatProviderReply reply;
    reply.success = false;
    reply.errorMessage = std::move(detail);
    reply.error = error;
    reply.category = category;
    reply.lifecycle =
        category == ChatProviderErrorCategory::Timeout       ? ChatRequestLifecycle::TimedOut
        : category == ChatProviderErrorCategory::Cancelled   ? ChatRequestLifecycle::Cancelled
        : category == ChatProviderErrorCategory::RateLimited ? ChatRequestLifecycle::RateLimited
                                                             : ChatRequestLifecycle::Failed;
    return reply;
}

} // namespace

OllamaChatProvider::OllamaChatProvider(OllamaConfig config, int timeoutMs)
    : config_(std::move(config)), timeoutMs_(timeoutMs) {}

QString OllamaChatProvider::name() const {
    return QStringLiteral("OllamaChatProvider");
}

QString OllamaChatProvider::endpoint() const {
    return config_.endpoint.toString();
}

QString OllamaChatProvider::selectedModel() const {
    return selectedModel_;
}

void OllamaChatProvider::setSelectedModel(const QString& model) {
    selectedModel_ = model.trimmed();
}

ChatProviderStatus OllamaChatProvider::status() const {
    if (!config_.endpoint.isLoopbackHttp()) {
        return ChatProviderStatus::Unavailable;
    }

    OllamaHttpRuntimeClient client(config_, std::min(timeoutMs_, 750));
    const auto health = client.healthCheck();
    if (health.healthStatus != OllamaHealthStatus::Healthy) {
        return ChatProviderStatus::Unavailable;
    }

    if (discoverySnapshot_ && !discoverySnapshot_->succeeded()) {
        return ChatProviderStatus::Unavailable;
    }

    return ChatProviderStatus::Ready;
}

ChatProviderReply OllamaChatProvider::sendMessage(const QString& message) {
    return sendMessageWithToken(message, {});
}

ChatProviderReply OllamaChatProvider::sendRequest(const QString& message,
                                                  const ChatRequestOptions& options) {
    if (options.nativeToolCalling || options.structuredOutput)
        return IChatProvider::sendRequest(message, options);
    return sendMessageWithToken(message, options.cancellationToken, options.images,
                                options.deadlineMs, options.maxOutputTokens, options.reasoning);
}

ChatProviderReply
OllamaChatProvider::sendMessageWithToken(const QString& message,
                                         const std::shared_ptr<std::atomic_bool>& cancellationToken,
                                         const QList<ChatImage>& images, int deadlineMs,
                                         int maxOutputTokens, ReasoningControl reasoning) {
    const auto trimmed = message.trimmed();
    if (trimmed.isEmpty()) {
        return failureReply(QStringLiteral("Prompt is blank."),
                            ChatProviderErrorCategory::RequestRejected);
    }

    const auto model = selectedModel_.trimmed();
    if (model.isEmpty()) {
        if (!discoverySnapshot_)
            return failureReply(
                QStringLiteral("An Ollama model must be selected for direct execution."),
                ChatProviderErrorCategory::RequestRejected);
        const auto& discovery = *discoverySnapshot_;
        if (!discovery.succeeded()) {
            auto reply = failureReply(discovery.safeDetail, discovery.errorCategory);
            reply.httpStatus = discovery.httpStatus;
            reply.attempts = discovery.attemptCount;
            reply.requestId = discovery.requestId;
            return reply;
        }
        if (discovery.models.isEmpty()) {
            return failureReply(
                QStringLiteral("No Ollama model is installed yet. "
                               "Run 'sentinel-cli model pull <name>' to install one."),
                ChatProviderErrorCategory::ModelNotFound);
        }
        return failureReply(QStringLiteral("An Ollama model must be selected explicitly."),
                            ChatProviderErrorCategory::ModelNotFound);
    }

    OllamaLocalInferenceClient inferenceClient(config_, timeoutMs_);
    LocalInferenceRequest request;
    request.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.prompt = trimmed;
    request.images = images;
    request.options.model = model;
    if (discoverySnapshot_)
        request.options.modelValidation = *discoverySnapshot_;
    request.options.timeoutMs = deadlineMs > 0 ? deadlineMs : timeoutMs_;
    request.options.cancellationToken = cancellationToken;
    request.options.temperature = 0.7;
    request.options.topP = 0.9;
    request.options.maxTokens = maxOutputTokens > 0 ? maxOutputTokens : 2048;
    // Ollama's documented reasoning switch; the transport writes it as `think`.
    request.options.reasoning = reasoning;
    request.options.reasoningEndpoint = ProviderEndpoint::Ollama;

    const auto response = inferenceClient.infer(request);
    if (response.status == LocalInferenceStatus::Succeeded) {
        ChatProviderReply reply{true, response.text, {}};
        reply.lifecycle = ChatRequestLifecycle::Completed;
        reply.attempts = response.attempts;
        reply.retrySummary = response.retrySummary;
        reply.requestId = response.requestId;
        reply.diagnostics = response.diagnostics;
        reply.completion = ResponseCompletion::Complete;
        return reply;
    }

    auto reply =
        failureReply(response.summary,
                     response.providerErrorCategory > 0
                         ? static_cast<ChatProviderErrorCategory>(response.providerErrorCategory)
                         : categoryFromLocalInference(response.error, response.status));
    reply.message = response.text;
    reply.httpStatus = response.httpStatus;
    reply.attempts = response.attempts;
    reply.retrySummary = response.retrySummary;
    reply.requestId = response.requestId;
    reply.diagnostics = response.diagnostics;
    if (response.diagnostics.value(QStringLiteral("output_limit_reached")).toBool())
        reply.completion = ResponseCompletion::Truncated;
    return reply;
}

ChatProviderReply OllamaChatProvider::sendMessageStreaming(
    const QString& message, const std::function<void(const QString&)>& onDelta,
    const std::shared_ptr<std::atomic_bool>& cancellationToken) {
    // Legacy streaming: no per-request options, provider-configured timeout only.
    return streamMessage(message, onDelta, cancellationToken, {}, timeoutMs_, 2048, false);
}

ChatProviderReply OllamaChatProvider::sendMessageStreaming(
    const QString& message, const std::function<void(const QString&)>& onDelta,
    const std::shared_ptr<std::atomic_bool>& cancellationToken, const ChatRequestOptions& options) {
    if (options.structuredOutput || options.nativeToolCalling) {
        auto reply = failureReply(
            QStringLiteral("Bounded streaming does not carry structured or native tool output."),
            ChatProviderErrorCategory::CapabilityUnsupported,
            ChatProviderReply::Error::CapabilityRejected);
        reply.diagnostics.insert(QStringLiteral("deadline_ms"), options.deadlineMs);
        reply.diagnostics.insert(QStringLiteral("bounded_streaming"),
                                 options.requireBoundedStreaming);
        return reply;
    }
    if (options.requireBoundedStreaming && options.deadlineMs <= 0) {
        // A bounded request must never run unbounded when its deadline is absent.
        auto reply = failureReply(
            QStringLiteral("Bounded streaming requires a positive request deadline."),
            ChatProviderErrorCategory::RequestRejected, ChatProviderReply::Error::InvalidResponse);
        reply.diagnostics.insert(QStringLiteral("deadline_ms"), 0);
        reply.diagnostics.insert(QStringLiteral("bounded_streaming"), true);
        return reply;
    }
    const auto token = cancellationToken ? cancellationToken : options.cancellationToken;
    const int deadlineMs = options.deadlineMs > 0 ? options.deadlineMs : timeoutMs_;
    const int maxOutputTokens = options.maxOutputTokens > 0 ? options.maxOutputTokens : 2048;
    return streamMessage(message, onDelta, token, options.images, deadlineMs, maxOutputTokens,
                         options.requireBoundedStreaming, options.reasoning);
}

ChatProviderReply OllamaChatProvider::streamMessage(
    const QString& message, const std::function<void(const QString&)>& onDelta,
    const std::shared_ptr<std::atomic_bool>& cancellationToken, const QList<ChatImage>& images,
    int deadlineMs, int maxOutputTokens, bool requireBoundedStreaming, ReasoningControl reasoning) {
    const auto trimmed = message.trimmed();
    if (trimmed.isEmpty())
        return failureReply(QStringLiteral("Prompt is blank."),
                            ChatProviderErrorCategory::RequestRejected);

    const auto model = selectedModel_.trimmed();
    if (model.isEmpty()) {
        if (!discoverySnapshot_)
            return failureReply(
                QStringLiteral("An Ollama model must be selected for direct execution."),
                ChatProviderErrorCategory::RequestRejected);
        const auto& discovery = *discoverySnapshot_;
        if (!discovery.succeeded()) {
            auto reply = failureReply(discovery.safeDetail, discovery.errorCategory);
            reply.httpStatus = discovery.httpStatus;
            reply.attempts = discovery.attemptCount;
            reply.requestId = discovery.requestId;
            return reply;
        }
        if (discovery.models.isEmpty()) {
            return failureReply(
                QStringLiteral("No Ollama model is installed yet. "
                               "Run 'sentinel-cli model pull <name>' to install one."),
                ChatProviderErrorCategory::ModelNotFound);
        }
        return failureReply(QStringLiteral("An Ollama model must be selected explicitly."),
                            ChatProviderErrorCategory::ModelNotFound);
    }
    if (discoverySnapshot_) {
        const auto& discovery = *discoverySnapshot_;
        if (!discovery.succeeded()) {
            auto reply = failureReply(discovery.safeDetail, discovery.errorCategory);
            reply.httpStatus = discovery.httpStatus;
            reply.attempts = discovery.attemptCount;
            reply.requestId = discovery.requestId;
            return reply;
        }
        if (discovery.models.isEmpty())
            return failureReply(QStringLiteral("No Ollama model is installed."),
                                ChatProviderErrorCategory::ModelNotFound);
        if (std::none_of(discovery.models.cbegin(), discovery.models.cend(),
                         [&](const OllamaModelSummary& item) { return item.name == model; }))
            return failureReply(QStringLiteral("Selected Ollama model is unavailable."),
                                ChatProviderErrorCategory::ModelNotFound);
    }

    LocalInferenceRequest request;
    request.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request.prompt = trimmed;
    request.images = images;
    request.options.model = model;
    if (discoverySnapshot_)
        request.options.modelValidation = *discoverySnapshot_;
    request.options.timeoutMs = deadlineMs;
    request.options.requireBoundedStreaming = requireBoundedStreaming;
    request.options.temperature = 0.7;
    request.options.topP = 0.9;
    request.options.maxTokens = maxOutputTokens;
    request.options.streamingRequested = true;
    request.options.cancellationToken = cancellationToken;
    request.options.reasoning = reasoning;
    request.options.reasoningEndpoint = ProviderEndpoint::Ollama;

    OllamaLocalInferenceStreamClient client(config_, timeoutMs_);
    const auto result = client.startStream(request, [&](const LocalInferenceStreamChunk& chunk) {
        if (!chunk.malformed && !chunk.text.isEmpty() &&
            !(cancellationToken && cancellationToken->load()) && onDelta) {
            onDelta(chunk.text);
        }
    });
    if (result.status == LocalInferenceStreamStatus::Completed) {
        ChatProviderReply reply{true, result.accumulatedText, {}};
        reply.lifecycle = ChatRequestLifecycle::Completed;
        reply.httpStatus = result.httpStatus;
        reply.attempts = result.attempts;
        reply.retrySummary = result.retrySummary;
        reply.requestId = result.requestId;
        reply.diagnostics.insert(QStringLiteral("deadline_ms"), result.timeoutMs);
        reply.diagnostics.insert(QStringLiteral("bounded_streaming"), requireBoundedStreaming);
        return reply;
    }
    auto reply =
        failureReply(result.summary,
                     result.providerErrorCategory > 0
                         ? static_cast<ChatProviderErrorCategory>(result.providerErrorCategory)
                     : result.status == LocalInferenceStreamStatus::Cancelled
                         ? ChatProviderErrorCategory::Cancelled
                         : categoryFromLocalInference(result.error, LocalInferenceStatus::Error));
    reply.message = result.accumulatedText;
    reply.httpStatus = result.httpStatus;
    reply.attempts = result.attempts;
    reply.retrySummary = result.retrySummary;
    reply.requestId = result.requestId;
    reply.diagnostics.insert(QStringLiteral("deadline_ms"), result.timeoutMs);
    reply.diagnostics.insert(QStringLiteral("bounded_streaming"), requireBoundedStreaming);
    return reply;
}

} // namespace sentinel::core
