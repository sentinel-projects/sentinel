// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/runtime/LocalInferenceTransport.h"
#include "sentinel/core/runtime/OllamaRuntime.h"

#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>

namespace sentinel::core {

enum class LocalInferenceStatus : std::uint8_t {
    NotRequested,
    Busy,
    Refused,
    Blocked,
    InvalidRequest,
    ModelUnavailable,
    Succeeded,
    Error,
};

QString localInferenceStatusName(LocalInferenceStatus status);

enum class LocalInferenceError : std::uint8_t {
    None,
    ClientUnavailable,
    OllamaNotRunning,
    EndpointUnreachable,
    BlankPrompt,
    MissingModel,
    EndpointBlocked,
    ModelUnavailable,
    PermissionDenied,
    SafetyBlocked,
    BusyRequest,
    RequestFailed,
    Timeout,
    InvalidResponse,
    StreamInterrupted,
    Offline,
};

QString localInferenceErrorName(LocalInferenceError error);

struct LocalInferenceOptions {
    struct NoPrevalidation {};
    QString model;
    std::variant<NoPrevalidation, OllamaModelDiscoveryResult> modelValidation = NoPrevalidation{};
    const OllamaModelDiscoveryResult* discoverySnapshot() const {
        return std::get_if<OllamaModelDiscoveryResult>(&modelValidation);
    }
    // Zero means no client-side timeout. Cancellation remains available.
    int timeoutMs = 0;
    double temperature = 0.7;
    double topP = 0.9;
    int maxTokens = 2048;
    bool streamingRequested = false;
    bool cancellationRequested = false;
    std::shared_ptr<std::atomic_bool> cancellationToken;
    // Explicit bounded requirement. When set, this streaming request must resolve
    // to a positive deadline; the transport refuses instead of waiting unbounded.
    bool requireBoundedStreaming = false;
};

struct LocalInferenceRequest {
    QString id = QStringLiteral("local-inference-request-1");
    QString prompt;
    LocalInferenceOptions options;
    QList<ChatImage> images;
};

struct LocalInferenceTrace {
    int sequence = 0;
    QString stage;
    QString status;
    QString summary;
};

struct LocalInferenceResponse {
    LocalInferenceStatus status = LocalInferenceStatus::NotRequested;
    LocalInferenceError error = LocalInferenceError::None;
    QString model;
    QString endpoint;
    QString text;
    QString summary = QStringLiteral("No local inference request yet.");
    qint64 latencyMs = -1;
    qint64 firstTokenLatencyMs = -1;
    int approximateOutputTokens = 0;
    double approximateTokensPerSecond = 0.0;
    QList<LocalInferenceTrace> traces;
    int timeoutMs = 0;
    int httpStatus = 0;
    int attempts = 1;
    QString retrySummary;
    QString requestId;
    int providerErrorCategory = 0;
    QJsonObject diagnostics;
};

enum class LocalInferenceStreamStatus : std::uint8_t {
    Disabled,
    NotStarted,
    Refused,
    Streaming,
    Completed,
    Cancelled,
    Error,
};

QString localInferenceStreamStatusName(LocalInferenceStreamStatus status);

struct LocalInferenceStreamChunk {
    int sequence = 0;
    QString text;
    bool finalChunk = false;
    bool malformed = false;
    QString summary;
};

struct LocalInferenceStreamResult {
    LocalInferenceStreamStatus status = LocalInferenceStreamStatus::Disabled;
    LocalInferenceError error = LocalInferenceError::None;
    QString summary = QStringLiteral("Local inference streaming is disabled.");
    QString model;
    QString endpoint;
    QString accumulatedText;
    bool cancelled = false;
    bool outputLimitReached = false;
    int malformedChunkCount = 0;
    qint64 latencyMs = -1;
    qint64 firstTokenLatencyMs = -1;
    int approximateOutputTokens = 0;
    double approximateTokensPerSecond = 0.0;
    QList<LocalInferenceStreamChunk> chunks;
    QList<LocalInferenceTrace> traces;
    int timeoutMs = 0;
    int httpStatus = 0;
    int providerErrorCategory = 0;
    QString requestId;
    int attempts = 1;
    QString retrySummary;
    ChatRequestLifecycle lifecycle = ChatRequestLifecycle::Pending;
};

QString localInferenceTraceSummary(const LocalInferenceTrace& trace);
QStringList localInferenceTraceSummaries(const QList<LocalInferenceTrace>& traces);
QString safeLocalInferenceSummary(const LocalInferenceResponse& response);
QString safeLocalInferenceResponseSummary(const LocalInferenceResponse& response);

class ILocalInferenceClient {
public:
    virtual ~ILocalInferenceClient() = default;

    virtual LocalInferenceResponse infer(const LocalInferenceRequest& request) = 0;
    virtual QString statusSummary() const = 0;
};

class ILocalInferenceStreamClient {
public:
    virtual ~ILocalInferenceStreamClient() = default;

    virtual LocalInferenceStreamResult
    startStream(const LocalInferenceRequest& request,
                const std::function<void(const LocalInferenceStreamChunk&)>& onChunk) = 0;
    virtual QString statusSummary() const = 0;
    virtual bool isAvailable() const = 0;
};

using LocalInferenceFinishedCallback =
    std::function<void(const QString&, const LocalInferenceResponse&)>;
using LocalInferenceStreamChunkCallback =
    std::function<void(const QString&, const LocalInferenceStreamChunk&)>;
using LocalInferenceStreamFinishedCallback =
    std::function<void(const QString&, const LocalInferenceStreamResult&)>;

class ILocalInferenceWorker {
public:
    virtual ~ILocalInferenceWorker() = default;

    virtual bool startInference(const LocalInferenceRequest& request,
                                LocalInferenceFinishedCallback onFinished) = 0;
    virtual bool startStream(const LocalInferenceRequest& request,
                             LocalInferenceStreamChunkCallback onChunk,
                             LocalInferenceStreamFinishedCallback onFinished) = 0;
    virtual void cancel(const QString& requestId) = 0;
    virtual QString statusSummary() const = 0;
    virtual QString streamStatusSummary() const = 0;
    virtual bool streamingAvailable() const = 0;
};

class NullLocalInferenceStreamClient final : public ILocalInferenceStreamClient {
public:
    LocalInferenceStreamResult
    startStream(const LocalInferenceRequest& request,
                const std::function<void(const LocalInferenceStreamChunk&)>& onChunk) override;
    QString statusSummary() const override;
    bool isAvailable() const override;
};

class NullLocalInferenceClient final : public ILocalInferenceClient {
public:
    LocalInferenceResponse infer(const LocalInferenceRequest& request) override;
    QString statusSummary() const override;
};

class LocalInferenceWorker final : public ILocalInferenceWorker {
public:
    LocalInferenceWorker(std::unique_ptr<ILocalInferenceClient> inferenceClient,
                         std::unique_ptr<ILocalInferenceStreamClient> streamClient,
                         QObject* callbackContext, bool threadedInference, bool threadedStream);
    ~LocalInferenceWorker() override;

    bool startInference(const LocalInferenceRequest& request,
                        LocalInferenceFinishedCallback onFinished) override;
    bool startStream(const LocalInferenceRequest& request,
                     LocalInferenceStreamChunkCallback onChunk,
                     LocalInferenceStreamFinishedCallback onFinished) override;
    void cancel(const QString& requestId) override;
    QString statusSummary() const override;
    QString streamStatusSummary() const override;
    bool streamingAvailable() const override;

private:
    bool hasActiveThread() const;
    bool requestCancelled(const LocalInferenceRequest& request) const;

    std::unique_ptr<ILocalInferenceClient> inferenceClient_;
    std::unique_ptr<ILocalInferenceStreamClient> streamClient_;
    QObject* callbackContext_ = nullptr;
    QThread* activeThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> activeCancellationToken_;
    bool threadedInference_ = false;
    bool threadedStream_ = false;
};

class OllamaLocalInferenceClient final : public ILocalInferenceClient {
public:
    explicit OllamaLocalInferenceClient(OllamaConfig config = OllamaConfig{}, int timeoutMs = 0);

    LocalInferenceResponse infer(const LocalInferenceRequest& request) override;
    QString statusSummary() const override;

private:
    QUrl endpointUrl(const QString& path) const;
    bool endpointAllowed() const;
    OllamaConfig config_;
    int timeoutMs_ = 0;
};

class OllamaLocalInferenceStreamClient final : public ILocalInferenceStreamClient {
public:
    explicit OllamaLocalInferenceStreamClient(OllamaConfig config = OllamaConfig{},
                                              int timeoutMs = 0);

    LocalInferenceStreamResult
    startStream(const LocalInferenceRequest& request,
                const std::function<void(const LocalInferenceStreamChunk&)>& onChunk) override;
    QString statusSummary() const override;
    bool isAvailable() const override;

private:
    QUrl endpointUrl(const QString& path) const;
    bool endpointAllowed() const;

    OllamaConfig config_;
    int timeoutMs_ = 0;
};

struct LMStudioConfig {
    QUrl endpoint = QUrl(QStringLiteral("http://127.0.0.1:1234"));
    int timeoutMs = 0;
    QString apiKey;
    // Safely constrained transport seam. Empty in production; a deterministic
    // fixture may inject a factory so the real request builders run against a
    // synthetic transport without contacting the network. See
    // LocalInferenceTransport.h.
    std::shared_ptr<ILocalInferenceNetworkFactory> networkFactory;

    static bool isLoopbackHttp(const QUrl& url) {
        QHostAddress address;
        return url.scheme() == QLatin1String("http") &&
               (url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 ||
                (address.setAddress(url.host()) && address.isLoopback()));
    }

    static bool isSecureTransportEndpoint(const QUrl& url) {
        return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
               !url.authority().contains(QLatin1Char('@')) &&
               (url.scheme() == QLatin1String("https") || isLoopbackHttp(url));
    }

    bool isAllowedEndpoint() const {
        return isSecureTransportEndpoint(endpoint) &&
               (isLoopbackHttp() || !apiKey.trimmed().isEmpty());
    }

    bool isLoopbackHttp() const {
        return isLoopbackHttp(endpoint);
    }

    bool isCloud() const {
        return !isLoopbackHttp();
    }

    QString providerDisplayName() const {
        const auto host = endpoint.host().toLower();
        if (host == QLatin1String("generativelanguage.googleapis.com"))
            return QStringLiteral("Google Gemini API");
        if (host == QLatin1String("api.anthropic.com"))
            return QStringLiteral("Anthropic Claude API");
        if (host == QLatin1String("api.openai.com"))
            return QStringLiteral("OpenAI API");
        if (host == QLatin1String("api.deepseek.com"))
            return QStringLiteral("DeepSeek API");
        if (host == QLatin1String("api.groq.com"))
            return QStringLiteral("Groq Cloud API");
        if (host == QLatin1String("api.mistral.ai"))
            return QStringLiteral("Mistral AI API");
        if (isLoopbackHttp())
            return QStringLiteral("LM Studio / Local Server");
        return QStringLiteral("Cloud API");
    }

    QString toString() const {
        return endpoint.toString(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment);
    }

    static LMStudioConfig defaultConfig() {
        return {};
    }
};

class LMStudioLocalInferenceClient final : public ILocalInferenceClient {
public:
    enum class NativeProtocol { Claude, Gemini };
    struct OpenAiCompletionResult {
        bool ok = false;
        QJsonObject body;
        QString error;
        int httpStatus = 0;
        QString retryAfter;
        QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
        bool timedOut = false;
        bool malformed = false;
        bool cancelled = false;
        int attempts = 1;
        QString retrySummary;
        QString requestId;
        int providerErrorCategory = 0;
    };
    explicit LMStudioLocalInferenceClient(LMStudioConfig config = LMStudioConfig{},
                                          int timeoutMs = 0);

    LocalInferenceResponse infer(const LocalInferenceRequest& request) override;
    OpenAiCompletionResult
    completeOpenAiChat(const QJsonObject& body,
                       const std::shared_ptr<std::atomic_bool>& cancellationToken = {}) const;
    OpenAiCompletionResult
    completeNativeChat(NativeProtocol protocol, const QJsonObject& body,
                       const std::shared_ptr<std::atomic_bool>& cancellationToken = {}) const;
    QString statusSummary() const override;

private:
    QUrl endpointUrl(const QString& path) const;
    bool endpointAllowed() const;

    LMStudioConfig config_;
    int timeoutMs_ = 0;
};

class LMStudioLocalInferenceStreamClient final : public ILocalInferenceStreamClient {
public:
    explicit LMStudioLocalInferenceStreamClient(LMStudioConfig config = LMStudioConfig{},
                                                int timeoutMs = 0);

    LocalInferenceStreamResult
    startStream(const LocalInferenceRequest& request,
                const std::function<void(const LocalInferenceStreamChunk&)>& onChunk) override;
    QString statusSummary() const override;
    bool isAvailable() const override;

private:
    QUrl endpointUrl(const QString& path) const;
    bool endpointAllowed() const;

    LMStudioConfig config_;
    int timeoutMs_ = 0;
};

} // namespace sentinel::core
