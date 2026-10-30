// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/runtime/ProviderRequestRuntime.h"
#include "sentinel/core/interfaces/IRetryPolicy.h"
#include "sentinel/core/runtime/TransportDiagnostics.h"

#include <QEventLoop>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

namespace sentinel::core {

ProviderTransportTrace::ProviderTransportTrace(bool requestCreatedHere) {
    clock_.start();
    frame_ = {{"attempt_id", ProviderRequestRuntime::requestId()}};
    if (requestCreatedHere)
        frame_.insert("request_created_ms", 0);
}

void ProviderTransportTrace::milestone(const char* key) {
    if (!frame_.contains(QLatin1String(key)))
        frame_.insert(QLatin1String(key), clock_.elapsed());
}

void ProviderTransportTrace::policyDecision(bool allowed) {
    frame_.insert("policy_allowed", allowed);
    if (allowed)
        milestone("policy_admitted_ms");
    else {
        frame_.insert("response_completed", false);
        frame_.insert("partial_body", false);
        milestone("observation_finished_ms");
    }
}

void ProviderTransportTrace::observeStatus(QNetworkReply* reply) {
    const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (status.isValid() && status.toInt() >= 100 && status.toInt() <= 599)
        frame_.insert("http_status", status.toInt());
}

void ProviderTransportTrace::observeReply(QNetworkReply* reply) {
    milestone("reply_created_ms");
    observeStatus(reply);
    QObject::connect(reply, &QNetworkReply::socketStartedConnecting, &context_,
                     [this] { milestone("socket_connect_started_ms"); });
    QObject::connect(reply, &QNetworkReply::encrypted, &context_,
                     [this] { milestone("tls_encrypted_ms"); });
    QObject::connect(reply, &QNetworkReply::requestSent, &context_,
                     [this] { milestone("request_sent_ms"); });
    QObject::connect(reply, &QNetworkReply::metaDataChanged, &context_, [this, reply] {
        milestone("metadata_received_ms");
        frame_.insert("metadata_signal_observed", true);
        observeStatus(reply);
    });
    QObject::connect(reply, &QNetworkReply::readyRead, &context_, [this, reply] {
        if (reply->bytesAvailable() > 0)
            milestone("first_body_ms");
    });
    QObject::connect(reply, &QNetworkReply::errorOccurred, &context_,
                     [this](QNetworkReply::NetworkError error) {
                         milestone("error_ms");
                         if (!frame_.contains("qt_network_error"))
                             frame_.insert("qt_network_error", int(error));
                     });
    QObject::connect(reply, &QNetworkReply::finished, &context_,
                     [this] { milestone("qt_finished_ms"); });
}

void ProviderTransportTrace::finish(QNetworkReply* reply, bool timedOut, bool cancelled) {
    observeStatus(reply);
    if (timedOut)
        milestone("deadline_expired_ms");
    if (cancelled)
        milestone("cancelled_ms");
    // finished() also fires on abort/error; it is not successful completion.
    const bool complete =
        reply->isFinished() && reply->error() == QNetworkReply::NoError && !timedOut && !cancelled;
    frame_.insert("response_completed", complete);
    frame_.insert("partial_body", frame_.contains("first_body_ms") && !complete);
    if (!frame_.contains("qt_network_error"))
        frame_.insert("qt_network_error", int(reply->error()));
    milestone("observation_finished_ms");
}

void ProviderTransportTrace::localStop(bool cancelled) {
    milestone(cancelled ? "cancelled_ms" : "deadline_expired_ms");
}

QJsonObject ProviderTransportTrace::snapshot() const {
    return sanitizedTransportDiagnostics(QJsonArray{frame_}).first().toObject();
}

QString ProviderRequestRuntime::requestId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

ChatProviderErrorCategory ProviderRequestRuntime::classify(
    int status, QNetworkReply::NetworkError error, bool timedOut, bool cancelled) {
    if (cancelled) return ChatProviderErrorCategory::Cancelled;
    // Validated HTTP status and explicit deadline/network timeouts outrank the Qt
    // network error code. Qt reports UnknownContentError for unmapped HTTP error
    // statuses (for example 408 and 429) as well as for genuinely unparseable bodies,
    // so the status must be interpreted before that fallback.
    if (timedOut || error == QNetworkReply::TimeoutError || status == 408)
        return ChatProviderErrorCategory::Timeout;
    if (status == 429) return ChatProviderErrorCategory::RateLimited;
    if (status == 401 || status == 403)
        return ChatProviderErrorCategory::AuthenticationRequired;
    if (status == 404) return ChatProviderErrorCategory::ModelNotFound;
    if (status == 400 || status == 422)
        return ChatProviderErrorCategory::RequestRejected;
    if (status == 501) return ChatProviderErrorCategory::CapabilityUnsupported;
    if (status >= 500)
        return ChatProviderErrorCategory::ProviderUnavailable;
    // Any remaining validated HTTP error status is a rejected request, not a
    // malformed body. Only a successful or absent HTTP status with an unparseable
    // body is a malformed response.
    if (status >= 400)
        return ChatProviderErrorCategory::RequestRejected;
    if (error == QNetworkReply::UnknownContentError)
        return ChatProviderErrorCategory::MalformedResponse;
    if (error != QNetworkReply::NoError)
        return ChatProviderErrorCategory::ConnectionFailed;
    return ChatProviderErrorCategory::None;
}

ProviderTransportResult ProviderRequestRuntime::wait(
    QNetworkReply* reply, int timeoutMs, const std::shared_ptr<std::atomic_bool>& cancellationToken,
    const std::function<void()>& onReadyRead, ProviderTransportTrace* trace,
    const std::shared_ptr<const OperationDeadline>& operation) {
    ProviderTransportResult result;
    ProviderTransportTrace localTrace(false);
    auto* observer = trace ? trace : &localTrace;
    observer->observeReply(reply);
    if (cancellationToken && cancellationToken->load()) {
        observer->localStop(true);
        reply->abort();
    }
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    bool deadlineExpired = false;
    if (operation && operation->expired()) {
        deadlineExpired = true;
        observer->localStop(false);
        reply->abort();
    }
    QTimer cancellationTimer;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
        deadlineExpired = true;
        observer->localStop(false);
        reply->abort();
        loop.quit();
    });
    if (onReadyRead)
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, onReadyRead);
    if (cancellationToken) {
        cancellationTimer.setInterval(25);
        QObject::connect(&cancellationTimer, &QTimer::timeout, &loop, [&] {
            if (cancellationToken->load()) {
                observer->localStop(true);
                reply->abort();
                loop.quit();
            }
        });
        cancellationTimer.start();
    }
    if (operation)
        timeoutMs = operation->childTimeoutMs(timeoutMs);
    if (operation && operation->bounded() && timeoutMs <= 0) {
        deadlineExpired = true;
        observer->localStop(false);
        reply->abort();
    }
    if (timeoutMs > 0)
        deadline.start(timeoutMs);
    if (!deadlineExpired && !reply->isFinished() &&
        !(cancellationToken && cancellationToken->load()))
        loop.exec();
    cancellationTimer.stop();
    result.cancelled = cancellationToken && cancellationToken->load();
    result.timedOut = !result.cancelled && (deadlineExpired || (operation && operation->expired()));
    if (result.timedOut || result.cancelled) reply->abort();
    result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.networkError = reply->error();
    result.retryAfter = QString::fromLatin1(reply->rawHeader("Retry-After"));
    result.category = classify(result.httpStatus, result.networkError,
                               result.timedOut, result.cancelled);
    result.lifecycle = result.cancelled ? ChatRequestLifecycle::Cancelled
                       : result.timedOut ? ChatRequestLifecycle::TimedOut
                       : result.category == ChatProviderErrorCategory::RateLimited
                           ? ChatRequestLifecycle::RateLimited
                       : result.category == ChatProviderErrorCategory::None
                           ? ChatRequestLifecycle::Completed
                           : ChatRequestLifecycle::Failed;
    observer->finish(reply, result.timedOut, result.cancelled);
    result.diagnostics = observer->snapshot();
    return result;
}

bool ProviderRequestRuntime::retryable(const ProviderTransportResult& result,
                                        ProviderRequestMode mode, bool outputEmitted) {
    if (result.cancelled || result.timedOut || outputEmitted) return false;
    const bool transientHttp = result.httpStatus == 429 || result.httpStatus == 502 ||
                               result.httpStatus == 503 || result.httpStatus == 504;
    const bool transientNetwork = result.httpStatus == 0 &&
        (result.networkError == QNetworkReply::TemporaryNetworkFailureError ||
         result.networkError == QNetworkReply::NetworkSessionFailedError ||
         result.networkError == QNetworkReply::ConnectionRefusedError ||
         result.networkError == QNetworkReply::HostNotFoundError);
    return (transientHttp || transientNetwork) &&
           (mode != ProviderRequestMode::HealthProbe || result.httpStatus != 429);
}

int ProviderRequestRuntime::retryDelayMs(int attempt, const QString& retryAfter,
                                          ProviderRequestMode mode) {
    RetryConfig config;
    config.maxRetries = mode == ProviderRequestMode::Discovery ||
                                mode == ProviderRequestMode::HealthProbe ? 1 : 2;
    config.initialDelayMs = 250;
    config.maxDelayMs = mode == ProviderRequestMode::Discovery ||
                                mode == ProviderRequestMode::HealthProbe ? 500 : 1500;
    return RetryPolicy(config).delayMs(attempt, retryAfter);
}

bool ProviderRequestRuntime::backoff(int delayMs,
                                     const std::shared_ptr<std::atomic_bool>& cancellationToken,
                                     const std::shared_ptr<const OperationDeadline>& operation) {
    if (cancellationToken && cancellationToken->load()) return false;
    if (operation && operation->expired())
        return false;
    if (operation)
        delayMs = operation->childTimeoutMs(delayMs);
    QEventLoop loop;
    QTimer::singleShot(qMax(0, delayMs), &loop, &QEventLoop::quit);
    QTimer cancellationTimer;
    if (cancellationToken) {
        cancellationTimer.setInterval(25);
        QObject::connect(&cancellationTimer, &QTimer::timeout, &loop, [&] {
            if (cancellationToken->load()) loop.quit();
        });
        cancellationTimer.start();
    }
    loop.exec();
    return (!cancellationToken || !cancellationToken->load()) &&
           (!operation || !operation->expired());
}

} // namespace sentinel::core
