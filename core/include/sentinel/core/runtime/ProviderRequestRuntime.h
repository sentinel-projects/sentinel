// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QNetworkReply>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>

namespace sentinel::core {

enum class ProviderRequestMode { Generation, StreamingGeneration, Discovery, HealthProbe };

struct ProviderTransportResult {
    ChatRequestLifecycle lifecycle = ChatRequestLifecycle::Pending;
    ChatProviderErrorCategory category = ChatProviderErrorCategory::None;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    int httpStatus = 0;
    QString retryAfter;
    bool cancelled = false;
    bool timedOut = false;
    QJsonObject diagnostics;
};

// Stack-owned observer; signal connections expire with the request. No payload reads.
class ProviderTransportTrace {
public:
    explicit ProviderTransportTrace(bool requestCreatedHere = true);
    void policyDecision(bool allowed);
    void observeReply(QNetworkReply* reply);
    void localStop(bool cancelled);
    void finish(QNetworkReply* reply, bool timedOut, bool cancelled);
    QJsonObject snapshot() const;

private:
    void milestone(const char* key);
    void observeStatus(QNetworkReply* reply);
    QElapsedTimer clock_;
    QObject context_;
    QJsonObject frame_;
};

class ProviderRequestRuntime {
public:
    static QString requestId();
    static ProviderTransportResult
    wait(QNetworkReply* reply, int timeoutMs,
         const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
         const std::function<void()>& onReadyRead = {}, ProviderTransportTrace* trace = nullptr,
         const std::shared_ptr<const OperationDeadline>& operation = {});
    static ChatProviderErrorCategory classify(int httpStatus,
                                               QNetworkReply::NetworkError error,
                                               bool timedOut = false,
                                               bool cancelled = false);
    static bool retryable(const ProviderTransportResult& result, ProviderRequestMode mode,
                          bool outputEmitted);
    static int retryDelayMs(int attempt, const QString& retryAfter, ProviderRequestMode mode);
    static bool backoff(int delayMs,
                        const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
                        const std::shared_ptr<const OperationDeadline>& operation = {});
};

} // namespace sentinel::core
