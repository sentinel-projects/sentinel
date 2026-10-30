// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/runtime/OperationDeadline.h"

#include "sentinel/core/runtime/ToolDescriptor.h"
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace sentinel::core {

enum class ChatProviderStatus {
    Unavailable,
    Ready,
    Error,
};

enum class ChatProviderConcurrency {
    Supported,
    Unsupported,
    Unknown,
};

enum class ChatRequestLifecycle {
    Pending,
    Running,
    Completed,
    Cancelled,
    TimedOut,
    RateLimited,
    Failed,
};

enum class ChatProviderErrorCategory {
    None,
    AuthenticationRequired,
    ModelNotFound,
    ProviderUnavailable,
    ConnectionFailed,
    Timeout,
    RateLimited,
    RequestRejected,
    CapabilityUnsupported,
    MalformedResponse,
    Cancelled,
    ProviderFailure,
    Offline,
};

inline QString chatProviderErrorCategoryName(ChatProviderErrorCategory category) {
    switch (category) {
    case ChatProviderErrorCategory::None:
        return QStringLiteral("None");
    case ChatProviderErrorCategory::AuthenticationRequired:
        return QStringLiteral("AuthenticationRequired");
    case ChatProviderErrorCategory::ModelNotFound:
        return QStringLiteral("ModelNotFound");
    case ChatProviderErrorCategory::ProviderUnavailable:
        return QStringLiteral("ProviderUnavailable");
    case ChatProviderErrorCategory::ConnectionFailed:
        return QStringLiteral("ConnectionFailed");
    case ChatProviderErrorCategory::Timeout:
        return QStringLiteral("Timeout");
    case ChatProviderErrorCategory::RateLimited:
        return QStringLiteral("RateLimited");
    case ChatProviderErrorCategory::RequestRejected:
        return QStringLiteral("RequestRejected");
    case ChatProviderErrorCategory::CapabilityUnsupported:
        return QStringLiteral("CapabilityUnsupported");
    case ChatProviderErrorCategory::MalformedResponse:
        return QStringLiteral("MalformedResponse");
    case ChatProviderErrorCategory::Cancelled:
        return QStringLiteral("Cancelled");
    case ChatProviderErrorCategory::ProviderFailure:
        return QStringLiteral("ProviderFailure");
    case ChatProviderErrorCategory::Offline:
        return QStringLiteral("Offline");
    }
    return QStringLiteral("None");
}

struct ProviderFailureMetadata {
    ChatRequestLifecycle lifecycle = ChatRequestLifecycle::Failed;
    ChatProviderErrorCategory category = ChatProviderErrorCategory::None;
    int httpStatus = 0;
    int attempts = 1;
    QString requestId;
};

// Typed response-completion state. `Truncated` means the provider stopped at its
// output limit and the visible answer is incomplete. A truncated reply must
// never be accepted as a finished classification, summary or final answer, even
// when it happens to contain parseable text.
enum class ResponseCompletion {
    // Provider reported a natural stop.
    Complete,
    // Provider stopped at its output limit; the reply is a prefix.
    Truncated,
    // Provider returned no usable payload at all.
    Empty,
};

// Caller's reasoning/thinking intent for one request. Providers translate this
// into the field their own protocol documents (Ollama `think`, llama.cpp
// `chat_template_kwargs`, LM Studio `reasoning_effort`); a protocol with no
// documented control emits nothing rather than borrowing another protocol's
// field name.
enum class ReasoningControl {
    // Leave the model's own default alone.
    Default,
    // No hidden reasoning budget. Used for classification and other small,
    // schema-shaped answers where reasoning only competes with the required JSON.
    Disabled,
    // Bounded reasoning; the model may think briefly.
    Low,
};

struct ChatProviderReply {
    bool success = false;
    QString message;
    QString errorMessage;
    enum class Error { None, CapabilityRejected, InvalidResponse, ProviderFailure };
    Error error = Error::None;
    ChatRequestLifecycle lifecycle = ChatRequestLifecycle::Pending;
    ChatProviderErrorCategory category = ChatProviderErrorCategory::None;
    int httpStatus = 0;
    int attempts = 1;
    QString retrySummary;
    QString requestId;
    struct ToolCall {
        QString callId;
        QString toolId;
        QJsonObject arguments;
        QString providerContinuation;
    };
    QList<ToolCall> toolCalls;
    std::optional<QJsonObject> structuredResult;
    // Allowlisted numeric/status metadata only; never prompts, credentials or reasoning.
    QJsonObject diagnostics;
    // Typed completion state normalized across providers. `Truncated` marks a
    // reply that stopped at the output limit; it is not a successful response.
    ResponseCompletion completion = ResponseCompletion::Complete;
};

struct ChatImage {
    QString mimeType;
    QByteArray bytes;
};

struct ChatRequestOptions {
    QList<ChatImage> images;
    std::shared_ptr<std::atomic_bool> cancellationToken;
    // Nonzero bounds are mandatory capabilities: providers must enforce or reject them.
    int deadlineMs = 0;
    int maxOutputTokens = 0;
    bool structuredOutput = false;
    bool nativeToolCalling = false;
    QString structuredSchemaName;
    QJsonObject structuredSchema;
    bool strictStructuredOutput = false;
    QList<ToolDescriptor> tools;
    QList<ChatProviderReply::ToolCall> priorToolCalls;
    struct ToolResult {
        QString callId;
        QString content;
    };
    QList<ToolResult> toolResults;
    // Budgeted production continuations preserve complete result units. Legacy/direct
    // callers retain the existing protocol preview limit unless they explicitly opt in.
    bool completeToolResults = false;
    // Explicit bounded-streaming requirement. A caller sets this when it needs the
    // provider to enforce a finite request deadline over the streaming transport.
    // A provider that cannot guarantee a finite deadline must reject instead of
    // running unbounded. A zero/absent deadline never satisfies a request that
    // explicitly requires bounded streaming.
    bool requireBoundedStreaming = false;
    // Reasoning/thinking intent. Providers map it to their own documented field.
    ReasoningControl reasoning = ReasoningControl::Default;
    std::shared_ptr<const OperationDeadline> operationDeadline;
};

inline std::optional<ChatProviderReply>
stoppedProviderOperation(const std::shared_ptr<std::atomic_bool>& token,
                         const std::shared_ptr<const OperationDeadline>& deadline) {
    const bool cancelled = token && token->load();
    if (!cancelled && (!deadline || !deadline->expired()))
        return {};
    ChatProviderReply reply;
    reply.category =
        cancelled ? ChatProviderErrorCategory::Cancelled : ChatProviderErrorCategory::Timeout;
    reply.lifecycle = cancelled ? ChatRequestLifecycle::Cancelled : ChatRequestLifecycle::TimedOut;
    reply.error = ChatProviderReply::Error::ProviderFailure;
    reply.errorMessage =
        cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Operation deadline expired.");
    reply.attempts = 0;
    return reply;
}

inline ChatProviderReply
enforceProviderOperationStop(ChatProviderReply reply,
                             const std::shared_ptr<std::atomic_bool>& token,
                             const std::shared_ptr<const OperationDeadline>& deadline) {
    if (auto stopped = stoppedProviderOperation(token, deadline)) {
        stopped->httpStatus = reply.httpStatus;
        stopped->attempts = reply.attempts;
        stopped->requestId = reply.requestId;
        stopped->diagnostics = reply.diagnostics;
        return *stopped;
    }
    return reply;
}

inline QString chatProviderStatusName(ChatProviderStatus status) {
    switch (status) {
    case ChatProviderStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case ChatProviderStatus::Ready:
        return QStringLiteral("Ready");
    case ChatProviderStatus::Error:
        return QStringLiteral("Error");
    }

    return QStringLiteral("Unavailable");
}

class IChatProvider {
public:
    Q_DISABLE_COPY(IChatProvider)
    IChatProvider() = default;
    virtual ~IChatProvider() = default;

    virtual QString name() const = 0;
    virtual ChatProviderStatus status() const = 0;
    virtual ChatProviderReply sendMessage(const QString& message) = 0;
    virtual ChatProviderReply sendRequest(const QString& message,
                                          const ChatRequestOptions& options) {
        if (auto stopped =
                stoppedProviderOperation(options.cancellationToken, options.operationDeadline))
            return *stopped;
        if (options.deadlineMs > 0 || options.maxOutputTokens > 0 || options.structuredOutput ||
            options.nativeToolCalling || !options.images.isEmpty() ||
            (options.operationDeadline && options.operationDeadline->bounded())) {
            ChatProviderReply reply;
            reply.errorMessage =
                QStringLiteral("Requested model capability is unavailable through this provider.");
            reply.error = ChatProviderReply::Error::CapabilityRejected;
            reply.category = ChatProviderErrorCategory::CapabilityUnsupported;
            reply.lifecycle = ChatRequestLifecycle::Failed;
            return reply;
        }
        return sendMessage(message);
    }
    virtual bool supportsStreaming() const {
        return false;
    }
    // Explicit bounded-streaming capability. Declaring or overriding streaming
    // support alone is not a bounded-streaming claim; a provider must opt in here
    // and implement the options-carrying streaming entry point below.
    virtual bool supportsBoundedStreaming() const {
        return false;
    }
    virtual ChatProviderConcurrency concurrency() const {
        return ChatProviderConcurrency::Unknown;
    }
    virtual ChatProviderReply sendMessageStreaming(const QString& message,
                                                   const std::function<void(const QString&)>&,
                                                   const std::shared_ptr<std::atomic_bool>&) {
        return sendMessage(message);
    }
    // Bounded streaming contract. Carries the caller's finite request deadline and
    // cancellation token into the streaming transport. Providers that cannot
    // enforce a finite deadline must not claim supportsBoundedStreaming(); this
    // default rejects explicitly rather than silently discarding the required
    // deadline or running unbounded.
    virtual ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken,
                         const ChatRequestOptions& options) {
        Q_UNUSED(message);
        Q_UNUSED(onDelta);
        Q_UNUSED(cancellationToken);
        Q_UNUSED(options);
        ChatProviderReply reply;
        reply.errorMessage =
            QStringLiteral("Bounded streaming is unavailable through this provider.");
        reply.error = ChatProviderReply::Error::CapabilityRejected;
        reply.category = ChatProviderErrorCategory::CapabilityUnsupported;
        reply.lifecycle = ChatRequestLifecycle::Failed;
        return reply;
    }
};

} // namespace sentinel::core

namespace sentinel::core::interfaces {
using IChatProvider = ::sentinel::core::IChatProvider;
using ChatProviderStatus = ::sentinel::core::ChatProviderStatus;
using ChatProviderReply = ::sentinel::core::ChatProviderReply;
} // namespace sentinel::core::interfaces
