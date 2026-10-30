// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelRouting.h"

#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <cstdint>
#include <atomic>
#include <memory>

class QNetworkAccessManager;

namespace sentinel::core {

class ILocalInferenceNetworkFactory;

enum class OllamaConnectionStatus : std::uint8_t {
    Unavailable,
    Blocked,
    Connected,
};

inline QString ollamaConnectionStatusName(OllamaConnectionStatus status) {
    switch (status) {
    case OllamaConnectionStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case OllamaConnectionStatus::Blocked:
        return QStringLiteral("Blocked");
    case OllamaConnectionStatus::Connected:
        return QStringLiteral("Connected");
    }

    return QStringLiteral("Unavailable");
}

enum class OllamaHealthStatus : std::uint8_t {
    Unavailable,
    InvalidEndpoint,
    Healthy,
    Error,
};

inline QString ollamaHealthStatusName(OllamaHealthStatus status) {
    switch (status) {
    case OllamaHealthStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case OllamaHealthStatus::InvalidEndpoint:
        return QStringLiteral("Invalid Endpoint");
    case OllamaHealthStatus::Healthy:
        return QStringLiteral("Healthy");
    case OllamaHealthStatus::Error:
        return QStringLiteral("Error");
    }

    return QStringLiteral("Unavailable");
}

struct OllamaEndpoint {
    QUrl url;
    bool valid = true;
    bool normalizedFromInvalid = false;

    static OllamaEndpoint defaultEndpoint();
    static OllamaEndpoint fromUserInput(const QString& endpoint);

    QString toString() const;
    bool isLoopbackHttp() const;
};

struct OllamaConfig {
    OllamaEndpoint endpoint = OllamaEndpoint::defaultEndpoint();
    bool healthCheckEnabled = true;
    bool modelDiscoveryEnabled = true;
    int healthCheckTimeoutMs = 750;
    int modelDiscoveryTimeoutMs = 1500;
    int generateTimeoutMs = 0;
    int streamTimeoutMs = 0;
    std::shared_ptr<ILocalInferenceNetworkFactory> networkFactory;

    static OllamaConfig fromEndpoint(const QString& endpoint);
};

struct OllamaModelSummary {
    QString name;
    QString modifiedAt;
    qint64 sizeBytes = 0;
    ModelCapabilities capabilities;
    QString family;
    QString publisher;
    QString architecture;
};

struct OllamaModelDiscoveryResult {
    QList<OllamaModelSummary> models;
    ChatRequestLifecycle lifecycle = ChatRequestLifecycle::Failed;
    ChatProviderErrorCategory errorCategory = ChatProviderErrorCategory::ProviderUnavailable;
    QString safeDetail;
    int httpStatus = 0;
    int attemptCount = 0;
    bool retried = false;
    QString requestId;

    bool succeeded() const { return lifecycle == ChatRequestLifecycle::Completed; }
};

struct OllamaHealthCheckResult {
    OllamaConnectionStatus connectionStatus = OllamaConnectionStatus::Unavailable;
    OllamaHealthStatus healthStatus = OllamaHealthStatus::Unavailable;
    QString endpoint;
    QString summary = QStringLiteral("Ollama runtime client is unavailable.");
    int timeoutMs = 0;
    ChatProviderErrorCategory providerErrorCategory = ChatProviderErrorCategory::None;
    int httpStatus = 0;
    QString requestId;
};

struct ProviderDiscoveryOutcome {
    bool completed = false;
    ChatProviderErrorCategory category = ChatProviderErrorCategory::None;
    int httpStatus = 0;
    QString requestId;
};

QString ollamaModelSummary(const OllamaModelSummary& model);
QStringList ollamaModelSummaries(const QList<OllamaModelSummary>& models);
QString safeOllamaHealthSummary(const OllamaHealthCheckResult& result);
QList<OllamaModelSummary> fetchOpenAiCompatibleModels(
    const QUrl& url, int timeoutMs, const QMap<QByteArray, QByteArray>& headers = {},
    QString* errorOut = nullptr, const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
    ProviderDiscoveryOutcome* outcome = nullptr,
    const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory = {});
QList<OllamaModelSummary>
fetchLlamaCppModels(const QUrl& modelsUrl, int timeoutMs,
                    const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
                    ProviderDiscoveryOutcome* outcome = nullptr);
QList<OllamaModelSummary>
fetchGeminiCloudModels(const QString& apiKey, int timeoutMs = 4000, QString* errorOut = nullptr,
                       const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
                       ProviderDiscoveryOutcome* outcome = nullptr,
                       const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory = {});
QList<OllamaModelSummary> fetchAnthropicCloudModels(
    const QString& apiKey, int timeoutMs = 4000, QString* errorOut = nullptr,
    const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
    ProviderDiscoveryOutcome* outcome = nullptr,
    const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory = {});
QList<OllamaModelSummary>
fetchOpenAiCloudModels(const QUrl& url, const QString& apiKey, int timeoutMs = 4000,
                       QString* errorOut = nullptr,
                       const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
                       ProviderDiscoveryOutcome* outcome = nullptr,
                       const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory = {});

class IOllamaRuntimeClient {
public:
    virtual ~IOllamaRuntimeClient() = default;

    virtual OllamaConfig config() const = 0;
    virtual OllamaHealthCheckResult healthCheck() const = 0;
    virtual QList<OllamaModelSummary> installedModels() const = 0;
    virtual OllamaHealthCheckResult healthCheck(
        const std::shared_ptr<std::atomic_bool>&) const { return healthCheck(); }
    virtual QList<OllamaModelSummary> installedModels(
        const std::shared_ptr<std::atomic_bool>&) const { return installedModels(); }
    virtual OllamaModelDiscoveryResult discoverModels(
        const std::shared_ptr<std::atomic_bool>& cancellationToken = {}) const {
        OllamaModelDiscoveryResult result;
        result.models = installedModels(cancellationToken);
        result.lifecycle = ChatRequestLifecycle::Completed;
        result.errorCategory = ChatProviderErrorCategory::None;
        return result;
    }
};

class NullOllamaRuntimeClient final : public IOllamaRuntimeClient {
public:
    explicit NullOllamaRuntimeClient(OllamaConfig config = OllamaConfig{});

    OllamaConfig config() const override;
    // Keep the cancellation-token overloads visible instead of hiding them.
    using IOllamaRuntimeClient::healthCheck;
    using IOllamaRuntimeClient::installedModels;
    OllamaHealthCheckResult healthCheck() const override;
    QList<OllamaModelSummary> installedModels() const override;
    OllamaModelDiscoveryResult discoverModels(
        const std::shared_ptr<std::atomic_bool>& cancellationToken = {}) const override;

private:
    OllamaConfig config_;
};

class OllamaHttpRuntimeClient final : public IOllamaRuntimeClient {
public:
    explicit OllamaHttpRuntimeClient(OllamaConfig config = OllamaConfig{}, int timeoutMs = 750);

    OllamaConfig config() const override;
    OllamaHealthCheckResult healthCheck() const override;
    QList<OllamaModelSummary> installedModels() const override;
    OllamaHealthCheckResult healthCheck(
        const std::shared_ptr<std::atomic_bool>& cancellationToken) const override;
    QList<OllamaModelSummary> installedModels(
        const std::shared_ptr<std::atomic_bool>& cancellationToken) const override;
    OllamaModelDiscoveryResult discoverModels(
        const std::shared_ptr<std::atomic_bool>& cancellationToken = {}) const override;

private:
    QUrl endpointUrl(const QString& path) const;
    bool endpointAllowed() const;

    OllamaConfig config_;
    int timeoutMs_ = 750;
};

} // namespace sentinel::core

// ── OllamaModelPuller ─────────────────────────────────────────────────────────
// QObject that drives a real Ollama /api/pull request and emits granular
// progress/completion signals so QML can drive live progress bars.
// Safe to expose to QML via setContextProperty.
// ─────────────────────────────────────────────────────────────────────────────
class OllamaModelPuller : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool pulling READ pulling NOTIFY pullingChanged)
    Q_PROPERTY(QString activeModel READ activeModel NOTIFY activeModelChanged)
    Q_PROPERTY(qreal progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusTextChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit OllamaModelPuller(QObject* parent = nullptr);

    bool pulling() const;
    QString activeModel() const;
    qreal progress() const;
    QString statusText() const;
    QString errorText() const;

    Q_INVOKABLE void pull(const QString& modelId);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void removeModel(const QString& modelId);
    Q_INVOKABLE void setEndpoint(const QString& endpoint);

signals:
    void pullingChanged();
    void activeModelChanged();
    void progressChanged();
    void statusTextChanged();
    void errorTextChanged();
    void pullFinished(const QString& modelId, bool success);
    void removeFinished(const QString& modelId, bool success);

private:
    void setState(bool pulling, const QString& model, qreal progress, const QString& status,
                  const QString& error);
    void processChunk(const QByteArray& chunk);

    bool pulling_ = false;
    QString activeModel_;
    qreal progress_ = 0.0;
    QString statusText_;
    QString errorText_;
    QString endpoint_ = QStringLiteral("http://127.0.0.1:11434");
    class QNetworkAccessManager* nam_ = nullptr;
    class QNetworkReply* reply_ = nullptr;
    QElapsedTimer pullTimer_;
    QByteArray pullBuffer_;
};

// ── OllamaLibraryFetcher ──────────────────────────────────────────────────────
class OllamaLibraryFetcher : public QObject {
    Q_OBJECT
    friend class OllamaRuntimeTest;
    Q_PROPERTY(bool fetching READ fetching NOTIFY fetchingChanged)
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit OllamaLibraryFetcher(QObject* parent = nullptr);

    QString fetchedAt() const {
        return fetchedAt_;
    }
    bool fetching() const;
    QVariantList models() const;
    QString errorText() const;

    Q_INVOKABLE void fetch(const QString& sort);
    Q_INVOKABLE void cancel();

signals:
    void fetchingChanged();
    void modelsChanged();
    void errorTextChanged();
    void fetchFinished(bool success);

private:
    void handleReplyFinished();
    void parseHtml(const QString& html);

    bool fetching_ = false;
    QVariantList models_;
    QString fetchedAt_;
    QString errorText_;
    class QNetworkAccessManager* nam_ = nullptr;
    class QNetworkReply* reply_ = nullptr;
};

// ── OllamaModelDetailFetcher ──────────────────────────────────────────────────
class OllamaModelDetailFetcher : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool fetching READ fetching NOTIFY fetchingChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(QString readme READ readme NOTIFY readmeChanged)
    Q_PROPERTY(QVariantList tags READ tags NOTIFY tagsChanged)
    Q_PROPERTY(QString installCmd READ installCmd NOTIFY installCmdChanged)

public:
    explicit OllamaModelDetailFetcher(QObject* parent = nullptr);

    bool fetching() const;
    QString errorText() const;
    QString readme() const;
    QVariantList tags() const;
    QString installCmd() const;

    Q_INVOKABLE void fetchDetails(const QString& modelId);
    Q_INVOKABLE void cancel();

signals:
    void fetchingChanged();
    void errorTextChanged();
    void readmeChanged();
    void tagsChanged();
    void installCmdChanged();
    void fetchFinished(bool success);

private:
    void handleReplyFinished();
    void parseHtml(const QString& html);
    QString decodeHtml(QString text);

    bool fetching_ = false;
    QString errorText_;
    QString readme_;
    QVariantList tags_;
    QString installCmd_;
    QString modelId_;
    class QNetworkAccessManager* nam_ = nullptr;
    class QNetworkReply* reply_ = nullptr;
};

// ── LMStudioLibraryFetcher ───────────────────────────────────────────────────
class LMStudioLibraryFetcher : public QObject {
    Q_OBJECT
    friend class OllamaRuntimeTest;
    Q_PROPERTY(bool fetching READ fetching NOTIFY fetchingChanged)
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)

public:
    explicit LMStudioLibraryFetcher(QObject* parent = nullptr);

    QString fetchedAt() const {
        return fetchedAt_;
    }
    bool fetching() const;
    QVariantList models() const;
    QString errorText() const;

    Q_INVOKABLE void fetch();
    Q_INVOKABLE void cancel();

signals:
    void fetchingChanged();
    void modelsChanged();
    void errorTextChanged();
    void fetchFinished(bool success);

private:
    void handleReplyFinished();
    void parseHtml(const QString& html);

    bool fetching_ = false;
    QVariantList models_;
    QString fetchedAt_;
    QString errorText_;
    class QNetworkAccessManager* nam_ = nullptr;
    class QNetworkReply* reply_ = nullptr;
};
