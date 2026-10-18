// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/runtime/OllamaRuntime.h"
#include "sentinel/core/model/ModelCategory.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/provider/protocols/anthropic_messages/AnthropicMessagesProtocol.h"
#include "sentinel/core/provider/protocols/gemini_generate_content/GeminiGenerateContentProtocol.h"
#include "sentinel/core/provider/protocols/openai_compatible/OpenAiCompatibleProtocol.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/runtime/LocalInferenceTransport.h"
#include "sentinel/core/runtime/ProviderRequestRuntime.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>
#include <QVariant>

#include <cmath>
#include <limits>
#include <utility>

namespace sentinel::core {

namespace {

QString catalogSnapshotPath(const QString& source) {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/model-catalog-" +
           source + ".json";
}
void loadCatalogSnapshot(const QString& source, QVariantList& models, QString& fetchedAt) {
    QFile file(catalogSnapshotPath(source));
    if (file.size() > 256 * 1024 || !file.open(QIODevice::ReadOnly))
        return;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    const auto timestamp = object.value("fetchedAt").toString();
    if (object.value("schema").toInt() != 1 || object.value("source").toString() != source ||
        !QDateTime::fromString(timestamp, Qt::ISODate).isValid())
        return;
    const auto records = object.value("models").toArray();
    for (const auto& record : records)
        if (!record.isObject() || record.toObject().value("id").toString().isEmpty())
            return;
    models = records.toVariantList();
    fetchedAt = timestamp;
}
void saveCatalogSnapshot(const QString& source, const QVariantList& models,
                         const QString& fetchedAt) {
    const auto bytes = QJsonDocument(QJsonObject{{"schema", 1},
                                                 {"source", source},
                                                 {"fetchedAt", fetchedAt},
                                                 {"models", QJsonArray::fromVariantList(models)}})
                           .toJson(QJsonDocument::Compact);
    if (bytes.size() > 256 * 1024)
        return;
    QDir().mkpath(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    QSaveFile file(catalogSnapshotPath(source));
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size())
        file.commit();
}

constexpr auto defaultOllamaEndpoint = "http://127.0.0.1:11434";

bool isLoopbackHost(const QString& host) {
    const auto normalized = host.trimmed().toLower();
    return normalized == QStringLiteral("127.0.0.1") || normalized == QStringLiteral("localhost") ||
           normalized == QStringLiteral("::1") || normalized == QStringLiteral("[::1]");
}

QUrl normalizedOllamaUrl(const QString& endpoint, bool* valid) {
    QUrl url = QUrl::fromUserInput(endpoint.trimmed());
    if (endpoint.trimmed().isEmpty()) {
        url = QUrl(QString::fromLatin1(defaultOllamaEndpoint));
    }

    const auto allowed = url.isValid() && url.scheme() == QStringLiteral("http") &&
                         isLoopbackHost(url.host()) && !url.hasQuery() && !url.hasFragment();
    if (!allowed) {
        if (valid) {
            *valid = false;
        }
        return QUrl(QString::fromLatin1(defaultOllamaEndpoint));
    }

    url.setPath(QString());
    if (url.port() < 0) {
        url.setPort(11434);
    }
    if (valid) {
        *valid = true;
    }
    return url;
}

struct JsonReply {
    bool ok = false;
    bool timedOut = false;
    QJsonDocument document;
    QString error;
    QNetworkReply::NetworkError networkError = QNetworkReply::NoError;
    int httpStatus = 0;
    QString retryAfter;
    ChatProviderErrorCategory category = ChatProviderErrorCategory::None;
    QString requestId;
    int attempts = 1;
};

JsonReply getJsonOnce(const QUrl& url, int timeoutMs, QNetworkAccessManager* manager,
                      const QMap<QByteArray, QByteArray>& headers = {},
                      const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
                      const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory = {}) {
    const auto decision = NetworkPolicyService::instance().check(url);
    if (decision != NetworkDecision::Allowed) {
        JsonReply blocked;
        blocked.category = ChatProviderErrorCategory::Offline;
        blocked.error = NetworkPolicyService::code(decision);
        return blocked;
    }
    if (!LMStudioConfig::isSecureTransportEndpoint(url)) {
        JsonReply blocked;
        blocked.category = ChatProviderErrorCategory::RequestRejected;
        blocked.error = QStringLiteral("Invalid or insecure model discovery endpoint.");
        return blocked;
    }
    std::unique_ptr<QNetworkAccessManager> localManager;
    if (!manager)
        localManager = networkFactory ? networkFactory->createManager()
                                      : std::make_unique<QNetworkAccessManager>();
    QNetworkAccessManager* activeManager = manager ? manager : localManager.get();
    QNetworkRequest request{url};
    // Credentialed discovery fails closed on redirects: no credential is replayed
    // to a new origin without an explicit per-hop policy boundary. Unauthenticated
    // Online probes retain their existing no-downgrade redirect policy.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         NetworkPolicyService::instance().mode() == NetworkMode::Online &&
                                 headers.isEmpty() &&
                                 !QUrlQuery(url).hasQueryItem(QStringLiteral("key"))
                             ? QNetworkRequest::NoLessSafeRedirectPolicy
                             : QNetworkRequest::ManualRedirectPolicy);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        request.setRawHeader(it.key(), it.value());
    }

    QNetworkReply* reply = activeManager->get(request);
    const auto transport = ProviderRequestRuntime::wait(reply, timeoutMs, cancellationToken);
    if (transport.cancelled) {
        reply->deleteLater();
        return JsonReply{false,
                         false,
                         {},
                         QStringLiteral("Request cancelled."),
                         QNetworkReply::OperationCanceledError};
    }
    if (transport.timedOut) {
        reply->deleteLater();
        return JsonReply{
            false, true, {}, QStringLiteral("Request timed out."), QNetworkReply::TimeoutError};
    }

    if (transport.category != ChatProviderErrorCategory::None) {
        const auto error = QStringLiteral("%1%2").arg(
            chatProviderErrorCategoryName(transport.category),
            transport.httpStatus > 0 ? QStringLiteral(" (HTTP %1)").arg(transport.httpStatus)
                                     : QString{});
        const auto networkError = reply->error();
        reply->deleteLater();
        return JsonReply{false,
                         false,
                         {},
                         error,
                         networkError,
                         transport.httpStatus,
                         transport.retryAfter,
                         transport.category};
    }

    if (transport.httpStatus >= 300 && transport.httpStatus < 400) {
        reply->deleteLater();
        return JsonReply{false,
                         false,
                         {},
                         QStringLiteral("Model discovery redirect rejected."),
                         QNetworkReply::NoError,
                         transport.httpStatus,
                         {},
                         ChatProviderErrorCategory::RequestRejected};
    }
    const auto payload = reply->readAll();
    reply->deleteLater();

    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return JsonReply{false,
                         false,
                         {},
                         QStringLiteral("Ollama local response was not valid JSON."),
                         QNetworkReply::UnknownContentError,
                         transport.httpStatus};
    }

    return JsonReply{true, false, document, {}, QNetworkReply::NoError, transport.httpStatus};
}

JsonReply getJson(const QUrl& url, int timeoutMs, QNetworkAccessManager* manager = nullptr,
                  const QMap<QByteArray, QByteArray>& headers = {},
                  const std::shared_ptr<std::atomic_bool>& cancellationToken = {},
                  ProviderRequestMode mode = ProviderRequestMode::Discovery,
                  const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory = {}) {
    const auto requestId = ProviderRequestRuntime::requestId();
    const int maxAttempts = mode == ProviderRequestMode::HealthProbe ? 1 : 2;
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (cancellationToken && cancellationToken->load()) {
            JsonReply cancelled;
            cancelled.category = ChatProviderErrorCategory::Cancelled;
            cancelled.requestId = requestId;
            cancelled.attempts = attempt;
            cancelled.error = QStringLiteral("Request cancelled.");
            return cancelled;
        }
        auto reply =
            getJsonOnce(url, timeoutMs, manager, headers, cancellationToken, networkFactory);
        reply.requestId = requestId;
        reply.attempts = attempt + 1;
        if (reply.category == ChatProviderErrorCategory::None)
            reply.category = ProviderRequestRuntime::classify(
                reply.httpStatus, reply.networkError, reply.timedOut,
                cancellationToken && cancellationToken->load());
        ProviderTransportResult transport;
        transport.httpStatus = reply.httpStatus;
        transport.networkError = reply.networkError;
        transport.timedOut = reply.timedOut;
        transport.cancelled = cancellationToken && cancellationToken->load();
        if (reply.ok || attempt + 1 == maxAttempts ||
            !ProviderRequestRuntime::retryable(transport, mode, false))
            return reply;
        if (!ProviderRequestRuntime::backoff(
                ProviderRequestRuntime::retryDelayMs(attempt, reply.retryAfter, mode),
                cancellationToken)) {
            JsonReply cancelled;
            cancelled.category = ChatProviderErrorCategory::Cancelled;
            cancelled.requestId = requestId;
            cancelled.attempts = attempt + 1;
            cancelled.error = QStringLiteral("Request cancelled.");
            return cancelled;
        }
    }
    return {};
}

QString safeOllamaNetworkFailureSummary(const JsonReply& reply, const QString& operation,
                                        int timeoutMs) {
    if (reply.timedOut) {
        return QStringLiteral("%1 timed out after %2 ms.").arg(operation).arg(timeoutMs);
    }

    if (reply.networkError == QNetworkReply::ConnectionRefusedError) {
        return QStringLiteral("%1 failed: Ollama is not running or is not listening locally.")
            .arg(operation);
    }

    if (reply.networkError == QNetworkReply::HostNotFoundError ||
        reply.networkError == QNetworkReply::NetworkSessionFailedError ||
        reply.networkError == QNetworkReply::TemporaryNetworkFailureError) {
        return QStringLiteral("%1 failed: local Ollama endpoint is unreachable.").arg(operation);
    }

    return QStringLiteral("%1 failed: %2").arg(operation, reply.error);
}

QString sizeSummary(qint64 sizeBytes) {
    if (sizeBytes <= 0) {
        return QStringLiteral("size unavailable");
    }

    constexpr qint64 kib = 1024;
    constexpr qint64 mib = kib * 1024;
    constexpr qint64 gib = mib * 1024;
    if (sizeBytes >= gib) {
        return QStringLiteral("%1 GiB").arg(
            QString::number(static_cast<double>(sizeBytes) / static_cast<double>(gib), 'f', 1));
    }
    if (sizeBytes >= mib) {
        return QStringLiteral("%1 MiB").arg(
            QString::number(static_cast<double>(sizeBytes) / static_cast<double>(mib), 'f', 1));
    }
    if (sizeBytes >= kib) {
        return QStringLiteral("%1 KiB").arg(
            QString::number(static_cast<double>(sizeBytes) / static_cast<double>(kib), 'f', 1));
    }
    return QStringLiteral("%1 B").arg(sizeBytes);
}

} // namespace

OllamaEndpoint OllamaEndpoint::defaultEndpoint() {
    return OllamaEndpoint{QUrl(QString::fromLatin1(defaultOllamaEndpoint)), true, false};
}

OllamaEndpoint OllamaEndpoint::fromUserInput(const QString& endpoint) {
    bool valid = true;
    const auto url = normalizedOllamaUrl(endpoint, &valid);
    return OllamaEndpoint{url, valid, !valid};
}

QString OllamaEndpoint::toString() const {
    auto text = url.toString();
    if (text.endsWith(QLatin1Char('/'))) {
        text.chop(1);
    }
    return text;
}

bool OllamaEndpoint::isLoopbackHttp() const {
    return valid && url.scheme() == QStringLiteral("http") && isLoopbackHost(url.host()) &&
           !url.hasQuery() && !url.hasFragment();
}

OllamaConfig OllamaConfig::fromEndpoint(const QString& endpoint) {
    OllamaConfig config;
    config.endpoint = OllamaEndpoint::fromUserInput(endpoint);
    return config;
}

QString ollamaModelSummary(const OllamaModelSummary& model) {
    QStringList details;
    details.append(sizeSummary(model.sizeBytes));
    if (!model.modifiedAt.isEmpty()) {
        details.append(QStringLiteral("modified %1").arg(model.modifiedAt));
    }
    details.append(QStringLiteral("Local Only"));
    return QStringLiteral("%1 (%2)").arg(model.name, details.join(QStringLiteral(", ")));
}

QStringList ollamaModelSummaries(const QList<OllamaModelSummary>& models) {
    QStringList summaries;
    for (const auto& model : models) {
        summaries.append(ollamaModelSummary(model));
    }
    return summaries;
}

QString safeOllamaHealthSummary(const OllamaHealthCheckResult& result) {
    if (!result.summary.isEmpty()) {
        return result.summary;
    }
    return QStringLiteral("Ollama health is %1 at %2.")
        .arg(ollamaHealthStatusName(result.healthStatus), result.endpoint);
}

NullOllamaRuntimeClient::NullOllamaRuntimeClient(OllamaConfig config)
    : config_(std::move(config)) {}

OllamaConfig NullOllamaRuntimeClient::config() const {
    return config_;
}

OllamaHealthCheckResult NullOllamaRuntimeClient::healthCheck() const {
    return OllamaHealthCheckResult{
        OllamaConnectionStatus::Unavailable,
        OllamaHealthStatus::Unavailable,
        config_.endpoint.toString(),
        QStringLiteral("Ollama runtime client is unavailable; no local health check was "
                       "performed."),
        config_.healthCheckTimeoutMs,
    };
}

QList<OllamaModelSummary> NullOllamaRuntimeClient::installedModels() const {
    return discoverModels().models;
}

OllamaModelDiscoveryResult NullOllamaRuntimeClient::discoverModels(
    const std::shared_ptr<std::atomic_bool>& cancellationToken) const {
    OllamaModelDiscoveryResult result;
    result.errorCategory = cancellationToken && cancellationToken->load()
                               ? ChatProviderErrorCategory::Cancelled
                               : ChatProviderErrorCategory::ProviderUnavailable;
    result.lifecycle = result.errorCategory == ChatProviderErrorCategory::Cancelled
                           ? ChatRequestLifecycle::Cancelled : ChatRequestLifecycle::Failed;
    result.safeDetail = QStringLiteral("Ollama runtime client is unavailable.");
    return result;
}

OllamaHttpRuntimeClient::OllamaHttpRuntimeClient(OllamaConfig config, int timeoutMs)
    : config_(std::move(config)), timeoutMs_(timeoutMs) {
    config_.healthCheckTimeoutMs = timeoutMs_;
    config_.modelDiscoveryTimeoutMs = timeoutMs_;
}

OllamaConfig OllamaHttpRuntimeClient::config() const {
    return config_;
}

OllamaHealthCheckResult OllamaHttpRuntimeClient::healthCheck() const {
    return healthCheck({});
}

OllamaHealthCheckResult OllamaHttpRuntimeClient::healthCheck(
    const std::shared_ptr<std::atomic_bool>& cancellationToken) const {
    if (!endpointAllowed()) {
        return OllamaHealthCheckResult{
            OllamaConnectionStatus::Blocked,
            OllamaHealthStatus::InvalidEndpoint,
            config_.endpoint.toString(),
            QStringLiteral("Ollama health check blocked: endpoint must be local loopback HTTP."),
            config_.healthCheckTimeoutMs,
            ChatProviderErrorCategory::RequestRejected,
        };
    }

    const auto timeoutMs =
        config_.healthCheckTimeoutMs > 0 ? config_.healthCheckTimeoutMs : timeoutMs_;
    const auto reply = getJson(endpointUrl(QStringLiteral("/api/version")), timeoutMs,
                               nullptr, {}, cancellationToken,
                               ProviderRequestMode::HealthProbe);
    if (!reply.ok) {
        return OllamaHealthCheckResult{
            OllamaConnectionStatus::Unavailable,
            OllamaHealthStatus::Error,
            config_.endpoint.toString(),
            safeOllamaNetworkFailureSummary(reply, QStringLiteral("Ollama local health check"),
                                            timeoutMs),
            timeoutMs,
            reply.category, reply.httpStatus, reply.requestId,
        };
    }

    return OllamaHealthCheckResult{
        OllamaConnectionStatus::Connected,
        OllamaHealthStatus::Healthy,
        config_.endpoint.toString(),
        QStringLiteral("Ollama local endpoint is reachable; no prompt or model execution was "
                       "performed."),
        timeoutMs,
        ChatProviderErrorCategory::None, reply.httpStatus, reply.requestId,
    };
}

QList<OllamaModelSummary> OllamaHttpRuntimeClient::installedModels() const {
    return installedModels({});
}

QList<OllamaModelSummary> OllamaHttpRuntimeClient::installedModels(
    const std::shared_ptr<std::atomic_bool>& cancellationToken) const {
    return discoverModels(cancellationToken).models;
}

OllamaModelDiscoveryResult OllamaHttpRuntimeClient::discoverModels(
    const std::shared_ptr<std::atomic_bool>& cancellationToken) const {
    OllamaModelDiscoveryResult result;
    if (!config_.modelDiscoveryEnabled || !endpointAllowed()) {
        result.errorCategory = ChatProviderErrorCategory::RequestRejected;
        result.safeDetail = config_.modelDiscoveryEnabled
                                ? QStringLiteral("Ollama discovery requires a local loopback HTTP endpoint.")
                                : QStringLiteral("Ollama model discovery is disabled.");
        return result;
    }

    const auto timeoutMs =
        config_.modelDiscoveryTimeoutMs > 0 ? config_.modelDiscoveryTimeoutMs : timeoutMs_;
    const auto reply = getJson(endpointUrl(QStringLiteral("/api/tags")), timeoutMs,
                               nullptr, {}, cancellationToken);
    result.requestId = reply.requestId;
    result.attemptCount = reply.attempts;
    result.retried = reply.attempts > 1;
    result.httpStatus = reply.httpStatus;
    if (!reply.ok) {
        result.errorCategory = reply.category;
        result.lifecycle = reply.category == ChatProviderErrorCategory::Timeout
                               ? ChatRequestLifecycle::TimedOut
                           : reply.category == ChatProviderErrorCategory::Cancelled
                               ? ChatRequestLifecycle::Cancelled
                           : reply.category == ChatProviderErrorCategory::RateLimited
                               ? ChatRequestLifecycle::RateLimited : ChatRequestLifecycle::Failed;
        result.safeDetail = safeOllamaNetworkFailureSummary(
            reply, QStringLiteral("Ollama model discovery"), timeoutMs);
        return result;
    }

    const auto modelsValue = reply.document.object().value(QStringLiteral("models"));
    if (!modelsValue.isArray()) {
        result.errorCategory = ChatProviderErrorCategory::MalformedResponse;
        result.safeDetail = QStringLiteral("Ollama model discovery returned an invalid model catalog.");
        return result;
    }
    const auto modelValues = modelsValue.toArray();
    for (const auto& value : modelValues) {
        const auto object = value.toObject();
        const auto name = object.value(QStringLiteral("name")).toString().trimmed();
        if (name.isEmpty()) {
            continue;
        }
        OllamaModelSummary model{
            name,
            object.value(QStringLiteral("modified_at")).toString(),
            object.value(QStringLiteral("size")).toVariant().toLongLong(),
        };
        const auto details = object.value(QStringLiteral("details")).toObject();
        model.family = details.value(QStringLiteral("family")).toString();
        result.models.append(std::move(model));
    }
    result.lifecycle = ChatRequestLifecycle::Completed;
    result.errorCategory = ChatProviderErrorCategory::None;
    result.safeDetail = result.models.isEmpty()
                            ? QStringLiteral("Ollama is reachable; no models are installed.")
                            : QStringLiteral("Ollama models are available.");
    return result;
}

QUrl OllamaHttpRuntimeClient::endpointUrl(const QString& path) const {
    QUrl url = config_.endpoint.url;
    url.setPath(path);
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

bool OllamaHttpRuntimeClient::endpointAllowed() const {
    return config_.healthCheckEnabled && config_.endpoint.isLoopbackHttp();
}

} // namespace sentinel::core

// ── OllamaModelPuller implementation ─────────────────────────────────────────

OllamaModelPuller::OllamaModelPuller(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

bool OllamaModelPuller::pulling() const {
    return pulling_;
}
QString OllamaModelPuller::activeModel() const {
    return activeModel_;
}
qreal OllamaModelPuller::progress() const {
    return progress_;
}
QString OllamaModelPuller::statusText() const {
    return statusText_;
}
QString OllamaModelPuller::errorText() const {
    return errorText_;
}

void OllamaModelPuller::setEndpoint(const QString& endpoint) {
    const auto normalized = sentinel::core::OllamaConfig::fromEndpoint(endpoint).endpoint;
    if (normalized.isLoopbackHttp()) {
        endpoint_ = normalized.toString();
    }
}

void OllamaModelPuller::pull(const QString& modelId) {
    if (pulling_) {
        cancel();
    }

    const QString trimmed = modelId.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    // Build request to local Ollama /api/pull
    QUrl url(endpoint_ + QStringLiteral("/api/pull"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);

    const QJsonObject body{{QStringLiteral("name"), trimmed}};
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    pullTimer_.start();
    pullBuffer_.clear();
    setState(true, trimmed, 0.0, QStringLiteral("Starting pull…"), QString());

    reply_ = nam_->post(request, payload);

    // Stream chunks as they arrive
    QObject::connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (!reply_)
            return;
        pullBuffer_.append(reply_->readAll());
        for (auto newline = pullBuffer_.indexOf('\n'); newline >= 0;
             newline = pullBuffer_.indexOf('\n')) {
            const auto line = pullBuffer_.left(newline).trimmed();
            pullBuffer_.remove(0, newline + 1);
            if (!line.isEmpty()) {
                processChunk(line);
            }
        }
    });

    QObject::connect(reply_, &QNetworkReply::finished, this, [this]() {
        if (!reply_)
            return;

        const bool networkError = (reply_->error() != QNetworkReply::NoError &&
                                   reply_->error() != QNetworkReply::OperationCanceledError);
        const bool cancelled = (reply_->error() == QNetworkReply::OperationCanceledError);
        const QString model = activeModel_;

        reply_->deleteLater();
        reply_ = nullptr;

        if (cancelled) {
            setState(false, model, 0.0, QStringLiteral("Cancelled."), QString());
            emit pullFinished(model, false);
            return;
        }

        if (!pullBuffer_.trimmed().isEmpty()) {
            processChunk(pullBuffer_.trimmed());
        }
        pullBuffer_.clear();

        if (networkError) {
            setState(false, model, 0.0, QString(),
                     QStringLiteral("Pull failed: Ollama is not running or unreachable."));
            emit pullFinished(model, false);
            return;
        }

        // If progress reached 1.0, it's done; otherwise mark complete
        if (progress_ >= 1.0 ||
            statusText_.contains(QStringLiteral("success"), Qt::CaseInsensitive)) {
            setState(false, model, 1.0, QStringLiteral("Installed"), QString());
            emit pullFinished(model, true);
        } else {
            setState(false, model, progress_, statusText_, errorText_);
            emit pullFinished(model, progress_ >= 0.99);
        }
    });
}

void OllamaModelPuller::cancel() {
    if (reply_) {
        reply_->abort();
    }
}

void OllamaModelPuller::removeModel(const QString& modelId) {
    const QString trimmed = modelId.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }

    QUrl url(endpoint_ + QStringLiteral("/api/delete"));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));

    const QJsonObject body{{QStringLiteral("name"), trimmed}};
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);

    QNetworkReply* reply = nam_->sendCustomRequest(request, "DELETE", payload);

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, trimmed]() {
        const bool success = (reply->error() == QNetworkReply::NoError);
        if (!success) {
            setState(
                false, trimmed, 0.0, QString(),
                QStringLiteral("Delete failed: Ollama is not running or rejected the request."));
        }
        reply->deleteLater();
        emit removeFinished(trimmed, success);
    });
}

void OllamaModelPuller::processChunk(const QByteArray& chunk) {
    QJsonParseError err;
    const auto doc = QJsonDocument::fromJson(chunk, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }

    const auto obj = doc.object();
    const auto status = obj.value(QStringLiteral("status")).toString();
    const qint64 completed = obj.value(QStringLiteral("completed")).toVariant().toLongLong();
    const qint64 total = obj.value(QStringLiteral("total")).toVariant().toLongLong();

    qreal newProgress = progress_;
    if (total > 0 && completed > 0) {
        newProgress = static_cast<qreal>(completed) / static_cast<qreal>(total);
    } else if (status.contains(QStringLiteral("success"), Qt::CaseInsensitive)) {
        newProgress = 1.0;
    }

    // Build a human-readable status
    QString displayStatus = status;
    if (!displayStatus.isEmpty()) {
        // Capitalise first letter
        displayStatus[0] = displayStatus[0].toUpper();
    }
    if (total > 0 && completed > 0 && total != completed) {
        const double pct = newProgress * 100.0;
        const double completedGb = static_cast<double>(completed) / (1024.0 * 1024.0 * 1024.0);
        const double totalGb = static_cast<double>(total) / (1024.0 * 1024.0 * 1024.0);

        QString speedStr;
        QString etaStr;
        qint64 elapsedMs = pullTimer_.isValid() ? pullTimer_.elapsed() : 0;
        if (elapsedMs > 500) {
            double elapsedSecs = elapsedMs / 1000.0;
            double speedBytesPerSec = static_cast<double>(completed) / elapsedSecs;

            if (speedBytesPerSec >= (1024.0 * 1024.0)) {
                speedStr =
                    QStringLiteral("%1 MB/s").arg(speedBytesPerSec / (1024.0 * 1024.0), 0, 'f', 1);
            } else {
                speedStr = QStringLiteral("%1 KB/s").arg(speedBytesPerSec / 1024.0, 0, 'f', 0);
            }

            if (speedBytesPerSec > 0.0) {
                double remainingBytes = static_cast<double>(total - completed);
                double remainingSecs = remainingBytes / speedBytesPerSec;
                int h = static_cast<int>(remainingSecs) / 3600;
                int m = (static_cast<int>(remainingSecs) % 3600) / 60;
                int s = static_cast<int>(remainingSecs) % 60;

                if (h > 0) {
                    etaStr = QStringLiteral("%1h %2m remaining").arg(h).arg(m);
                } else if (m > 0) {
                    etaStr = QStringLiteral("%1m %2s remaining").arg(m).arg(s);
                } else {
                    etaStr = QStringLiteral("%1s remaining").arg(s);
                }
            }
        }

        QString baseStatus = status;
        if (!baseStatus.isEmpty()) {
            baseStatus[0] = baseStatus[0].toUpper();
        }

        QString sizeInfo;
        if (totalGb >= 0.1) {
            sizeInfo =
                QStringLiteral("%1 GB / %2 GB").arg(completedGb, 0, 'f', 2).arg(totalGb, 0, 'f', 2);
        } else {
            const double completedMb = static_cast<double>(completed) / (1024.0 * 1024.0);
            const double totalMb = static_cast<double>(total) / (1024.0 * 1024.0);
            sizeInfo =
                QStringLiteral("%1 MB / %2 MB").arg(completedMb, 0, 'f', 1).arg(totalMb, 0, 'f', 1);
        }

        if (!speedStr.isEmpty() && !etaStr.isEmpty()) {
            displayStatus = QStringLiteral("%1 — %2% (%3 • %4 • %5)")
                                .arg(baseStatus)
                                .arg(static_cast<int>(pct))
                                .arg(sizeInfo)
                                .arg(speedStr)
                                .arg(etaStr);
        } else {
            displayStatus = QStringLiteral("%1 — %2% (%3)")
                                .arg(baseStatus)
                                .arg(static_cast<int>(pct))
                                .arg(sizeInfo);
        }
    }

    if (qAbs(newProgress - progress_) > 0.001 || displayStatus != statusText_) {
        pulling_ = true;
        progress_ = newProgress;
        statusText_ = displayStatus;
        errorText_.clear();
        emit progressChanged();
        emit statusTextChanged();
        if (!pulling_) {
            pulling_ = true;
            emit pullingChanged();
        }
    }
}

void OllamaModelPuller::setState(bool pulling, const QString& model, qreal progress,
                                 const QString& status, const QString& error) {
    bool changed = false;

    if (pulling_ != pulling) {
        pulling_ = pulling;
        emit pullingChanged();
        changed = true;
    }
    if (activeModel_ != model) {
        activeModel_ = model;
        emit activeModelChanged();
        changed = true;
    }
    if (qAbs(progress_ - progress) > 0.0001) {
        progress_ = progress;
        emit progressChanged();
        changed = true;
    }
    if (statusText_ != status) {
        statusText_ = status;
        emit statusTextChanged();
        changed = true;
    }
    if (errorText_ != error) {
        errorText_ = error;
        emit errorTextChanged();
        changed = true;
    }

    Q_UNUSED(changed)
}

namespace {
QString catalogPlainText(QString value) {
    value.replace("&amp;", "&");
    value.replace("&#39;", "'");
    value.replace("&#x27;", "'");
    value.replace("&quot;", "\"");
    value.replace("&lt;", "<");
    value.replace("&gt;", ">");
    value.replace("&nbsp;", " ");
    return value;
}
} // namespace

// ── OllamaLibraryFetcher implementation ───────────────────────────────────────

OllamaLibraryFetcher::OllamaLibraryFetcher(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    sentinel::core::loadCatalogSnapshot("ollama", models_, fetchedAt_);
}

bool OllamaLibraryFetcher::fetching() const {
    return fetching_;
}
QVariantList OllamaLibraryFetcher::models() const {
    return models_;
}
QString OllamaLibraryFetcher::errorText() const {
    return errorText_;
}

void OllamaLibraryFetcher::fetch(const QString& sort) {
    if (fetching_) {
        cancel();
    }

    fetching_ = true;
    errorText_.clear();
    emit fetchingChanged();
    emit errorTextChanged();

    QString sortParam = sort.trimmed().toLower();
    if (sortParam != QStringLiteral("newest")) {
        sortParam = QStringLiteral("popular");
    }

    QUrl url(QStringLiteral("https://ollama.com/library?sort=") + sortParam);
    QNetworkRequest request(url);
    request.setTransferTimeout(12000);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Sentinel Assistant)"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    reply_ = nam_->get(request);

    auto* currentReply = reply_;
    connect(currentReply, &QNetworkReply::finished, this, [this, currentReply] {
        if (reply_ != currentReply) {
            currentReply->deleteLater();
            return;
        }
        handleReplyFinished();
    });
}

void OllamaLibraryFetcher::cancel() {
    if (reply_) {
        reply_->abort();
    }
}

void OllamaLibraryFetcher::handleReplyFinished() {
    if (!reply_)
        return;

    QNetworkReply* r = reply_;
    reply_ = nullptr;
    r->deleteLater();

    fetching_ = false;
    emit fetchingChanged();

    if (r->error() != QNetworkReply::NoError) {
        if (r->error() == QNetworkReply::OperationCanceledError) {
            errorText_ = QStringLiteral("Fetch cancelled.");
        } else {
            errorText_ = QStringLiteral("Failed to fetch Ollama models: %1").arg(r->errorString());
        }
        emit errorTextChanged();
        emit fetchFinished(false);
        return;
    }

    const QString html = QString::fromUtf8(r->readAll());
    parseHtml(html);
    if (errorText_.isEmpty()) {
        fetchedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        sentinel::core::saveCatalogSnapshot("ollama", models_, fetchedAt_);
    }
    emit fetchFinished(errorText_.isEmpty());
}

void OllamaLibraryFetcher::parseHtml(const QString& html) {
    QVariantList parsedList;
    int index = 0;
    while (true) {
        int nextLink = html.indexOf(QStringLiteral("href=\"/library/"), index);
        if (nextLink == -1) {
            break;
        }

        int startQuote = nextLink + 15;
        int endQuote = html.indexOf(QStringLiteral("\""), startQuote);
        if (endQuote == -1) {
            index = startQuote;
            continue;
        }

        QString modelPath = html.mid(startQuote, endQuote - startQuote);
        if (modelPath.contains('/') || modelPath.contains('?')) {
            index = endQuote;
            continue;
        }

        int liStart = html.lastIndexOf(QStringLiteral("<li"), nextLink);
        if (liStart == -1)
            liStart = nextLink;

        int liEnd = html.indexOf(QStringLiteral("</li>"), nextLink);
        if (liEnd == -1)
            liEnd = nextLink + 2000;

        QString block = html.mid(liStart, liEnd - liStart);
        index = liEnd;

        QString ollamaId = modelPath;

        // Name
        int nameStart = block.indexOf(QStringLiteral("class=\"group-hover:underline truncate\">"));
        QString name = ollamaId;
        if (nameStart != -1) {
            nameStart += 39;
            int nameEnd = block.indexOf(QStringLiteral("</span>"), nameStart);
            if (nameEnd != -1) {
                name = block.mid(nameStart, nameEnd - nameStart).trimmed();
            }
        }

        // Description
        int descStart = block.indexOf(
            QStringLiteral("class=\"max-w-lg break-words text-neutral-800 text-md\">"));
        if (descStart == -1) {
            descStart = block.indexOf(QStringLiteral("text-neutral-800"));
            if (descStart != -1) {
                descStart = block.indexOf(QStringLiteral(">"), descStart);
                if (descStart != -1)
                    descStart += 1;
            }
        } else {
            descStart += 54;
        }
        QString description;
        if (descStart != -1) {
            int descEnd = block.indexOf(QStringLiteral("</p>"), descStart);
            if (descEnd != -1) {
                description = block.mid(descStart, descEnd - descStart).trimmed();
                description.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
            }
        }

        // Capabilities
        QStringList caps;
        int capIndex = 0;
        while (true) {
            int capStart = block.indexOf(QStringLiteral("x-test-capability"), capIndex);
            if (capStart == -1)
                break;
            capStart = block.indexOf(QStringLiteral(">"), capStart);
            if (capStart == -1)
                break;
            capStart += 1;
            int capEnd = block.indexOf(QStringLiteral("</span>"), capStart);
            if (capEnd == -1)
                break;
            QString cap = block.mid(capStart, capEnd - capStart).trimmed();
            if (!cap.isEmpty() && !caps.contains(cap)) {
                caps.append(cap);
            }
            capIndex = capEnd;
        }

        // Pull count
        QString pulls = QStringLiteral("—");
        int pullStart = block.indexOf(QStringLiteral("x-test-pull-count>"));
        if (pullStart != -1) {
            pullStart += 18;
            int pullEnd = block.indexOf(QStringLiteral("</span>"), pullStart);
            if (pullEnd != -1) {
                pulls = block.mid(pullStart, pullEnd - pullStart).trimmed();
            }
        }

        // Updated time
        QString updated = QStringLiteral("—");
        int updatedStart = block.indexOf(QStringLiteral("x-test-updated>"));
        if (updatedStart != -1) {
            updatedStart += 15;
            int updatedEnd = block.indexOf(QStringLiteral("</span>"), updatedStart);
            if (updatedEnd != -1) {
                updated = block.mid(updatedStart, updatedEnd - updatedStart).trimmed();
            }
        }

        const auto category = sentinel::core::modelCategory(ollamaId, caps);

        QString provider = QStringLiteral("Ollama Library");
        QString idLower = ollamaId.toLower();
        if (idLower.startsWith(QStringLiteral("llama")) ||
            idLower.startsWith(QStringLiteral("codellama"))) {
            provider = QStringLiteral("Meta");
        } else if (idLower.startsWith(QStringLiteral("gemma")) ||
                   idLower.startsWith(QStringLiteral("codegemma"))) {
            provider = QStringLiteral("Google");
        } else if (idLower.startsWith(QStringLiteral("qwen"))) {
            provider = QStringLiteral("Alibaba");
        } else if (idLower.startsWith(QStringLiteral("deepseek"))) {
            provider = QStringLiteral("DeepSeek");
        } else if (idLower.startsWith(QStringLiteral("mistral")) ||
                   idLower.startsWith(QStringLiteral("mixtral"))) {
            provider = QStringLiteral("Mistral AI");
        } else if (idLower.startsWith(QStringLiteral("phi"))) {
            provider = QStringLiteral("Microsoft");
        } else if (idLower.startsWith(QStringLiteral("smollm"))) {
            provider = QStringLiteral("Hugging Face");
        } else if (idLower.contains(QStringLiteral("nemotron"))) {
            provider = QStringLiteral("NVIDIA");
        }

        QVariantMap modelObj;
        modelObj[QStringLiteral("id")] = ollamaId;
        modelObj[QStringLiteral("ollamaId")] = ollamaId;
        modelObj[QStringLiteral("category")] = category;
        modelObj[QStringLiteral("name")] = catalogPlainText(name);
        modelObj[QStringLiteral("provider")] = provider;
        modelObj[QStringLiteral("catalogSource")] = QStringLiteral("ollama");
        modelObj[QStringLiteral("size")] = QStringLiteral("—");
        modelObj[QStringLiteral("popularity")] = pulls + QStringLiteral(" pulls");
        modelObj[QStringLiteral("description")] = catalogPlainText(description);
        modelObj[QStringLiteral("externalUrl")] =
            QStringLiteral("https://ollama.com/library/") + ollamaId;

        QString badge = caps.isEmpty() ? QStringLiteral("Ollama") : caps.first();
        QString badgeColor = QStringLiteral("#7c3aed");
        if (badge == QStringLiteral("tools")) {
            badgeColor = QStringLiteral("#10b981");
        } else if (badge == QStringLiteral("thinking")) {
            badgeColor = QStringLiteral("#2563eb");
        } else if (badge == QStringLiteral("vision")) {
            badgeColor = QStringLiteral("#e05fc4");
        }
        modelObj[QStringLiteral("badge")] = badge;
        modelObj[QStringLiteral("badgeColor")] = badgeColor;

        QStringList tagsList = caps;
        if (updated != QStringLiteral("—")) {
            tagsList.append(updated);
        }
        modelObj[QStringLiteral("tags")] = tagsList;
        modelObj[QStringLiteral("downloadable")] = true;

        parsedList.append(modelObj);
    }

    if (parsedList.isEmpty()) {
        errorText_ = QStringLiteral("Catalog page could not be parsed; previous results retained.");
        emit errorTextChanged();
        return;
    }
    models_ = parsedList;
    emit modelsChanged();
}

// ── OllamaModelDetailFetcher implementation ───────────────────────────────────

OllamaModelDetailFetcher::OllamaModelDetailFetcher(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {}

bool OllamaModelDetailFetcher::fetching() const {
    return fetching_;
}
QString OllamaModelDetailFetcher::errorText() const {
    return errorText_;
}
QString OllamaModelDetailFetcher::readme() const {
    return readme_;
}
QVariantList OllamaModelDetailFetcher::tags() const {
    return tags_;
}
QString OllamaModelDetailFetcher::installCmd() const {
    return installCmd_;
}

void OllamaModelDetailFetcher::fetchDetails(const QString& modelId) {
    if (fetching_) {
        cancel();
    }

    fetching_ = true;
    errorText_.clear();
    readme_.clear();
    tags_.clear();
    installCmd_.clear();
    modelId_ = modelId.trimmed();

    emit fetchingChanged();
    emit errorTextChanged();
    emit readmeChanged();
    emit tagsChanged();
    emit installCmdChanged();

    if (modelId_.isEmpty()) {
        fetching_ = false;
        emit fetchingChanged();
        emit fetchFinished(false);
        return;
    }

    QUrl url(QStringLiteral("https://ollama.com/library/") + modelId_.toLower());
    QNetworkRequest request(url);
    request.setTransferTimeout(12000);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Sentinel Assistant)"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    reply_ = nam_->get(request);

    connect(reply_, &QNetworkReply::finished, this, &OllamaModelDetailFetcher::handleReplyFinished);
}

void OllamaModelDetailFetcher::cancel() {
    if (reply_) {
        reply_->abort();
    }
}

void OllamaModelDetailFetcher::handleReplyFinished() {
    if (!reply_)
        return;

    QNetworkReply* r = reply_;
    reply_ = nullptr;
    r->deleteLater();

    fetching_ = false;
    emit fetchingChanged();

    if (r->error() != QNetworkReply::NoError) {
        if (r->error() == QNetworkReply::OperationCanceledError) {
            errorText_ = QStringLiteral("Fetch cancelled.");
        } else {
            errorText_ = QStringLiteral("Failed to fetch model details: %1").arg(r->errorString());
        }
        emit errorTextChanged();
        emit fetchFinished(false);
        return;
    }

    const QString html = QString::fromUtf8(r->readAll());
    parseHtml(html);
    emit fetchFinished(true);
}

QString OllamaModelDetailFetcher::decodeHtml(QString text) {
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#34;"), QLatin1String("\""));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&#x27;"), QLatin1String("'"));
    return text;
}

void OllamaModelDetailFetcher::parseHtml(const QString& html) {
    // 1. Extract Readme (from <textarea id="editor")
    int textareaStart = html.indexOf(QStringLiteral("<textarea id=\"editor\""));
    if (textareaStart == -1) {
        textareaStart = html.indexOf(QStringLiteral("<textarea")); // Fallback
    }

    QString readmeText;
    if (textareaStart != -1) {
        int contentStart = html.indexOf(QStringLiteral(">"), textareaStart);
        if (contentStart != -1) {
            contentStart += 1;
            int textareaEnd = html.indexOf(QStringLiteral("</textarea>"), contentStart);
            if (textareaEnd != -1) {
                readmeText = html.mid(contentStart, textareaEnd - contentStart);
                readmeText = decodeHtml(readmeText).trimmed();
            }
        }
    }

    // If textarea not found, let's try to extract from the display div
    if (readmeText.isEmpty()) {
        int displayStart = html.indexOf(QStringLiteral("id=\"display\""));
        if (displayStart != -1) {
            displayStart = html.indexOf(QStringLiteral(">"), displayStart);
            if (displayStart != -1) {
                displayStart += 1;
                int displayEnd =
                    html.indexOf(QStringLiteral("</div>"), displayStart); // A rough end
                if (displayEnd != -1) {
                    readmeText = html.mid(displayStart, displayEnd - displayStart);
                    // Strip html tags
                    readmeText.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
                    readmeText = decodeHtml(readmeText).trimmed();
                }
            }
        }
    }

    readme_ = readmeText;

    // 2. Extract Tags & Sizes
    QVariantList tagsList;
    QString searchStr =
        QStringLiteral("href=\"/library/") + modelId_.toLower() + QStringLiteral(":");
    int tagIndex = 0;
    while (true) {
        int pos = html.indexOf(searchStr, tagIndex);
        if (pos == -1)
            break;

        int startQuote = pos + 6; // points to "/library/..."
        int endQuote = html.indexOf(QStringLiteral("\""), startQuote);
        if (endQuote == -1)
            break;

        QString fullTagPath = html.mid(startQuote, endQuote - startQuote); // "/library/llama3.2:1b"
        int colonIdx = fullTagPath.indexOf(':');
        QString tagOnly;
        if (colonIdx != -1) {
            tagOnly = fullTagPath.mid(colonIdx + 1);
        } else {
            tagOnly = QStringLiteral("latest");
        }

        // Find size
        QString sizeStr = QStringLiteral("—");
        int sizePos = html.indexOf(QStringLiteral("x-test-model-tag-size"), endQuote);
        if (sizePos != -1 && sizePos - endQuote < 1000) {
            int sizeTextStart = html.indexOf(QStringLiteral(">"), sizePos);
            if (sizeTextStart != -1) {
                sizeTextStart += 1;
                int sizeTextEnd = html.indexOf(QStringLiteral("</p>"), sizeTextStart);
                if (sizeTextEnd != -1) {
                    sizeStr = html.mid(sizeTextStart, sizeTextEnd - sizeTextStart).trimmed();
                }
            }
        }

        QVariantMap tagObj;
        tagObj[QStringLiteral("tag")] = tagOnly;
        tagObj[QStringLiteral("fullTag")] = modelId_ + QStringLiteral(":") + tagOnly;
        tagObj[QStringLiteral("size")] = sizeStr;

        bool exists = false;
        for (const auto& existing : tagsList) {
            if (existing.toMap()[QStringLiteral("tag")].toString() == tagOnly) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            tagsList.append(tagObj);
        }

        tagIndex = endQuote;
    }

    tags_ = tagsList;

    // 3. Extract install command
    QString command = QStringLiteral("ollama run ") + modelId_;
    int cliPanel = html.indexOf(QStringLiteral("data-panel=\"cli\""));
    if (cliPanel != -1) {
        int preStart = html.indexOf(QStringLiteral("<pre"), cliPanel);
        if (preStart != -1) {
            int preTextStart = html.indexOf(QStringLiteral(">"), preStart);
            if (preTextStart != -1) {
                preTextStart += 1;
                int preTextEnd = html.indexOf(QStringLiteral("</pre>"), preTextStart);
                if (preTextEnd != -1) {
                    command = html.mid(preTextStart, preTextEnd - preTextStart).trimmed();
                }
            }
        }
    }
    installCmd_ = command;

    emit readmeChanged();
    emit tagsChanged();
    emit installCmdChanged();
}

// ── LMStudioLibraryFetcher implementation ─────────────────────────────────────

LMStudioLibraryFetcher::LMStudioLibraryFetcher(QObject* parent)
    : QObject(parent), nam_(new QNetworkAccessManager(this)) {
    sentinel::core::loadCatalogSnapshot("lmstudio", models_, fetchedAt_);
}

bool LMStudioLibraryFetcher::fetching() const {
    return fetching_;
}
QVariantList LMStudioLibraryFetcher::models() const {
    return models_;
}
QString LMStudioLibraryFetcher::errorText() const {
    return errorText_;
}

void LMStudioLibraryFetcher::fetch() {
    if (fetching_) {
        cancel();
    }

    fetching_ = true;
    errorText_.clear();
    emit fetchingChanged();
    emit errorTextChanged();

    QUrl url(QStringLiteral("https://lmstudio.ai/models?sort=updated"));
    QNetworkRequest request(url);
    request.setTransferTimeout(12000);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Sentinel Assistant)"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    reply_ = nam_->get(request);

    auto* currentReply = reply_;
    connect(currentReply, &QNetworkReply::finished, this, [this, currentReply] {
        if (reply_ != currentReply) {
            currentReply->deleteLater();
            return;
        }
        handleReplyFinished();
    });
}

void LMStudioLibraryFetcher::cancel() {
    if (reply_) {
        reply_->abort();
    }
}

void LMStudioLibraryFetcher::handleReplyFinished() {
    if (!reply_)
        return;

    QNetworkReply* r = reply_;
    reply_ = nullptr;
    r->deleteLater();

    fetching_ = false;
    emit fetchingChanged();

    if (r->error() != QNetworkReply::NoError) {
        if (r->error() == QNetworkReply::OperationCanceledError) {
            errorText_ = QStringLiteral("Fetch cancelled.");
        } else {
            errorText_ =
                QStringLiteral("Failed to fetch LM Studio models: %1").arg(r->errorString());
        }
        emit errorTextChanged();
        emit fetchFinished(false);
        return;
    }

    const QString html = QString::fromUtf8(r->readAll());
    parseHtml(html);
    if (errorText_.isEmpty()) {
        fetchedAt_ = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        sentinel::core::saveCatalogSnapshot("lmstudio", models_, fetchedAt_);
    }
    emit fetchFinished(errorText_.isEmpty());
}

void LMStudioLibraryFetcher::parseHtml(const QString& html) {
    QVariantList parsedList;
    int index = 0;
    while (true) {
        int nextLink = html.indexOf(QStringLiteral("href=\"/models/"), index);
        if (nextLink == -1) {
            break;
        }

        int startQuote = nextLink + 14;
        int endQuote = html.indexOf(QStringLiteral("\""), startQuote);
        if (endQuote == -1) {
            index = startQuote;
            continue;
        }

        QString modelId = html.mid(startQuote, endQuote - startQuote);
        if (modelId.contains('/') || modelId.contains('?')) {
            index = endQuote;
            continue;
        }

        int aStart = html.lastIndexOf(QStringLiteral("<a"), nextLink);
        if (aStart == -1)
            aStart = nextLink;

        int aEnd = html.indexOf(QStringLiteral("</a>"), nextLink);
        if (aEnd == -1)
            aEnd = nextLink + 2000;

        QString block = html.mid(aStart, aEnd - aStart);
        index = aEnd;

        // 1. Name
        int nameStart = block.indexOf(QStringLiteral("class=\"text-lg font-medium\">"));
        QString name = modelId;
        if (nameStart != -1) {
            nameStart += 28;
            int nameEnd = block.indexOf(QStringLiteral("</div>"), nameStart);
            if (nameEnd != -1) {
                name = block.mid(nameStart, nameEnd - nameStart).trimmed();
            }
        }

        // 2. Sizes / Tags
        QStringList tags;
        int tagPos = 0;
        while (true) {
            tagPos = block.indexOf(QStringLiteral("font-mono text-xs"), tagPos);
            if (tagPos == -1)
                break;

            int textStart = block.indexOf(QStringLiteral(">"), tagPos);
            if (textStart == -1)
                break;
            textStart += 1;

            int textEnd = block.indexOf(QStringLiteral("</div>"), textStart);
            if (textEnd == -1)
                break;

            QString tagVal = block.mid(textStart, textEnd - textStart).trimmed();
            tagVal.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
            tagVal = catalogPlainText(tagVal).simplified();
            if (!tagVal.isEmpty() && !tags.contains(tagVal)) {
                tags.append(tagVal);
            }

            tagPos = textEnd;
        }

        // 3. Description
        int descStart = block.indexOf(QStringLiteral("text-muted-foreground"));
        QString description;
        if (descStart != -1) {
            descStart = block.indexOf(QStringLiteral(">"), descStart);
            if (descStart != -1) {
                descStart += 1;
                int descEnd = block.indexOf(QStringLiteral("</div>"), descStart);
                if (descEnd != -1) {
                    description = block.mid(descStart, descEnd - descStart).trimmed();
                    description.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
                }
            }
        }

        // 4. Downloads count
        int downloadsStart = block.indexOf(QStringLiteral("<span class=\"font-medium\">"));
        QString downloads = QStringLiteral("—");
        QString stars = QStringLiteral("—");
        if (downloadsStart != -1) {
            int dStart = downloadsStart + 26;
            int dEnd = block.indexOf(QStringLiteral("</span>"), dStart);
            if (dEnd != -1) {
                downloads = block.mid(dStart, dEnd - dStart).trimmed();

                int starsStart =
                    block.indexOf(QStringLiteral("<span class=\"font-medium\">"), dEnd);
                if (starsStart != -1) {
                    starsStart += 26;
                    int starsEnd = block.indexOf(QStringLiteral("</span>"), starsStart);
                    if (starsEnd != -1) {
                        stars = block.mid(starsStart, starsEnd - starsStart).trimmed();
                    }
                }
            }
        }

        // 5. Updated time
        int updatedStart = block.indexOf(QStringLiteral("Updated"));
        QString updated = QStringLiteral("—");
        if (updatedStart != -1) {
            int updatedEnd = block.indexOf(QStringLiteral("</div>"), updatedStart);
            if (updatedEnd != -1) {
                updated = block.mid(updatedStart, updatedEnd - updatedStart).trimmed();
                updated.remove(QRegularExpression(QStringLiteral("<!--[^>]*-->")));
                updated = updated.simplified();
            }
        }

        QStringList capabilities;
        const auto lowerDescription = description.toLower();
        if (lowerDescription.contains("vision") || lowerDescription.contains("multimodal"))
            capabilities << "vision";
        if (lowerDescription.contains("reasoning") || lowerDescription.contains("thinking"))
            capabilities << "reasoning";
        if (lowerDescription.contains("embedding"))
            capabilities << "embedding";
        const auto category = sentinel::core::modelCategory(modelId, capabilities);

        // Infer real provider from model ID slug
        QString lmsProvider = QStringLiteral("Community");
        QString lmsIdLower = modelId.toLower();
        if (lmsIdLower.contains(QStringLiteral("meta")) ||
            lmsIdLower.contains(QStringLiteral("llama"))) {
            lmsProvider = QStringLiteral("Meta");
        } else if (lmsIdLower.contains(QStringLiteral("google")) ||
                   lmsIdLower.contains(QStringLiteral("gemma"))) {
            lmsProvider = QStringLiteral("Google");
        } else if (lmsIdLower.contains(QStringLiteral("qwen")) ||
                   lmsIdLower.contains(QStringLiteral("alibaba"))) {
            lmsProvider = QStringLiteral("Alibaba");
        } else if (lmsIdLower.contains(QStringLiteral("deepseek"))) {
            lmsProvider = QStringLiteral("DeepSeek");
        } else if (lmsIdLower.contains(QStringLiteral("mistral")) ||
                   lmsIdLower.contains(QStringLiteral("mixtral"))) {
            lmsProvider = QStringLiteral("Mistral AI");
        } else if (lmsIdLower.contains(QStringLiteral("microsoft")) ||
                   lmsIdLower.contains(QStringLiteral("phi"))) {
            lmsProvider = QStringLiteral("Microsoft");
        } else if (lmsIdLower.contains(QStringLiteral("nvidia")) ||
                   lmsIdLower.contains(QStringLiteral("nemotron"))) {
            lmsProvider = QStringLiteral("NVIDIA");
        } else if (lmsIdLower.contains(QStringLiteral("huggingface")) ||
                   lmsIdLower.contains(QStringLiteral("smollm"))) {
            lmsProvider = QStringLiteral("Hugging Face");
        } else if (lmsIdLower.contains(QStringLiteral("bartowski")) ||
                   lmsIdLower.contains(QStringLiteral("unsloth")) ||
                   lmsIdLower.contains(QStringLiteral("mradermacher")) ||
                   lmsIdLower.contains(QStringLiteral("lmstudio-community"))) {
            lmsProvider = QStringLiteral("LM Studio Community");
        }

        QVariantMap modelObj;
        modelObj[QStringLiteral("id")] = QStringLiteral("lmstudio-") + modelId;
        modelObj[QStringLiteral("ollamaId")] = QStringLiteral("");
        modelObj[QStringLiteral("category")] = category;
        modelObj[QStringLiteral("name")] = catalogPlainText(name);
        modelObj[QStringLiteral("provider")] = lmsProvider;
        modelObj[QStringLiteral("catalogSource")] = QStringLiteral("lmstudio");
        modelObj[QStringLiteral("size")] = QStringLiteral("—");
        modelObj[QStringLiteral("popularity")] = downloads + QStringLiteral(" downloads");
        modelObj[QStringLiteral("cloudOnly")] =
            !block.contains(QStringLiteral("Available to download")) &&
            block.contains(QStringLiteral("Available in LM Studio Cloud"));
        modelObj[QStringLiteral("description")] = catalogPlainText(description);
        if (modelObj.value(QStringLiteral("cloudOnly")).toBool())
            modelObj[QStringLiteral("runtimeNote")] =
                QStringLiteral("Cloud-only catalog entry; no local download is advertised.");
        modelObj[QStringLiteral("badge")] = QStringLiteral("LM Studio");
        modelObj[QStringLiteral("badgeColor")] = QStringLiteral("#3b82f6");

        QStringList tagsList = tags + capabilities;
        if (updated != QStringLiteral("—")) {
            tagsList.append(updated);
        }
        if (stars != QStringLiteral("—")) {
            tagsList.append(stars + QStringLiteral(" ★"));
        }
        modelObj[QStringLiteral("tags")] = tagsList;
        modelObj[QStringLiteral("downloadable")] = false;
        modelObj[QStringLiteral("externalUrl")] =
            QStringLiteral("https://lmstudio.ai/models/") + modelId;

        parsedList.append(modelObj);
    }

    if (parsedList.isEmpty()) {
        errorText_ = QStringLiteral("Catalog page could not be parsed; previous results retained.");
        emit errorTextChanged();
        return;
    }
    models_ = parsedList;
    emit modelsChanged();
}

namespace sentinel::core {
static QList<OllamaModelSummary>
fetchModelCatalog(const QUrl& url, int timeoutMs, const QMap<QByteArray, QByteArray>& headers,
                  QString* errorOut, const std::shared_ptr<std::atomic_bool>& cancellationToken,
                  ProviderDiscoveryOutcome* outcome,
                  const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory,
                  std::optional<QList<OllamaModelSummary>> (*parseCatalog)(const QJsonObject&)) {
    const auto reply = getJson(url, timeoutMs, nullptr, headers, cancellationToken,
                               ProviderRequestMode::Discovery, networkFactory);
    if (outcome)
        *outcome = {false, reply.category, reply.httpStatus, reply.requestId};
    if (!reply.ok) {
        if (errorOut) {
            *errorOut = reply.timedOut ? QStringLiteral("Timed out after %1 ms").arg(timeoutMs)
                                       : safeOllamaNetworkFailureSummary(
                                             reply, QStringLiteral("Model list"), timeoutMs);
        }
        return {};
    }

    const auto parsed = parseCatalog(reply.document.object());
    if (!parsed) {
        if (outcome)
            *outcome = {false, ChatProviderErrorCategory::MalformedResponse, reply.httpStatus,
                        reply.requestId};
        if (errorOut)
            *errorOut = QStringLiteral("Model list returned an invalid model catalog.");
        return {};
    }
    if (outcome)
        *outcome = {true, ChatProviderErrorCategory::None, reply.httpStatus, reply.requestId};
    if (errorOut)
        errorOut->clear();
    return *parsed;
}

QList<OllamaModelSummary> fetchOpenAiCompatibleModels(
    const QUrl& url, int timeoutMs, const QMap<QByteArray, QByteArray>& headers, QString* errorOut,
    const std::shared_ptr<std::atomic_bool>& cancellationToken, ProviderDiscoveryOutcome* outcome,
    const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory) {
    return fetchModelCatalog(
        url, timeoutMs, headers, errorOut, cancellationToken, outcome, networkFactory,
        [](const QJsonObject& body) -> std::optional<QList<OllamaModelSummary>> {
            auto parsed = provider::parseOpenAiCompatibleModelCatalog(body);
            if (parsed.malformed)
                return std::nullopt;
            return std::move(parsed.models);
        });
}

QList<OllamaModelSummary>
fetchLlamaCppModels(const QUrl& modelsUrl, int timeoutMs,
                    const std::shared_ptr<std::atomic_bool>& cancellationToken,
                    ProviderDiscoveryOutcome* outcome) {
    auto models =
        fetchOpenAiCompatibleModels(modelsUrl, timeoutMs, {}, nullptr, cancellationToken, outcome);
    if (models.size() != 1 || (cancellationToken && cancellationToken->load()))
        return models;
    auto propsUrl = modelsUrl;
    auto path = propsUrl.path();
    const auto suffix = QStringLiteral("/v1/models");
    if (!path.endsWith(suffix))
        return models;
    path.chop(suffix.size());
    propsUrl.setPath(path + QStringLiteral("/props"));
    const auto reply = getJson(propsUrl, timeoutMs, nullptr, {}, cancellationToken);
    if (!reply.ok)
        return models; // Optional metadata must not invalidate working Chat discovery.
    const auto props = reply.document.object();
    // /props describes the loaded runtime, not every model in a router catalog.
    // Bind it only to an unambiguous, matching loaded-model identity.
    if (props.value(QStringLiteral("model_alias")).toString() != models.front().name)
        return models;
    const auto caps = props.value(QStringLiteral("chat_template_caps")).toObject();
    const auto tools = caps.value(QStringLiteral("supports_tools"));
    const auto calls = caps.value(QStringLiteral("supports_tool_calls"));
    if (tools.isBool() && calls.isBool())
        models.front().capabilities.nativeToolCalling = tools.toBool() && calls.toBool()
                                                            ? CapabilitySupport::Supported
                                                            : CapabilitySupport::Unsupported;
    const auto context = props.value(QStringLiteral("default_generation_settings"))
                             .toObject()
                             .value(QStringLiteral("n_ctx"))
                             .toDouble();
    if (context > 0 && context <= std::numeric_limits<int>::max() && context == std::floor(context))
        models.front().capabilities.contextWindow = static_cast<int>(context);
    return models;
}

QList<OllamaModelSummary>
fetchGeminiCloudModels(const QString& apiKey, int timeoutMs, QString* errorOut,
                       const std::shared_ptr<std::atomic_bool>& cancellationToken,
                       ProviderDiscoveryOutcome* outcome,
                       const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory) {
    if (apiKey.trimmed().isEmpty()) {
        if (outcome)
            *outcome = {false, ChatProviderErrorCategory::AuthenticationRequired, 0, {}};
        if (errorOut) {
            *errorOut = QStringLiteral("Gemini API key is empty.");
        }
        return {};
    }
    QUrl url(QStringLiteral("https://generativelanguage.googleapis.com/v1beta/models"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("key"), apiKey.trimmed());
    url.setQuery(query);
    const auto reply = getJson(url, timeoutMs, nullptr, {}, cancellationToken,
                               ProviderRequestMode::Discovery, networkFactory);
    if (outcome)
        *outcome = {reply.ok, reply.category, reply.httpStatus, reply.requestId};
    if (!reply.ok) {
        if (errorOut) {
            *errorOut = reply.timedOut ? QStringLiteral("Timed out after %1 ms").arg(timeoutMs)
                                       : safeOllamaNetworkFailureSummary(
                                             reply, QStringLiteral("Gemini API"), timeoutMs);
        }
        return {};
    }

    const auto malformedCatalog = [&]() -> QList<OllamaModelSummary> {
        if (outcome)
            *outcome = {false, ChatProviderErrorCategory::MalformedResponse, reply.httpStatus,
                        reply.requestId};
        if (errorOut)
            *errorOut = QStringLiteral("Gemini returned an invalid model catalog.");
        return {};
    };
    const auto parsed = provider::parseGeminiModelCatalog(reply.document.object());
    return parsed ? *parsed : malformedCatalog();
}

QList<OllamaModelSummary>
fetchAnthropicCloudModels(const QString& apiKey, int timeoutMs, QString* errorOut,
                          const std::shared_ptr<std::atomic_bool>& cancellationToken,
                          ProviderDiscoveryOutcome* outcome,
                          const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory) {
    if (apiKey.trimmed().isEmpty()) {
        if (outcome)
            *outcome = {false, ChatProviderErrorCategory::AuthenticationRequired, 0, {}};
        if (errorOut) {
            *errorOut = QStringLiteral("Anthropic API key is empty.");
        }
        return {};
    }
    const QUrl url(QStringLiteral("https://api.anthropic.com/v1/models"));
    const QMap<QByteArray, QByteArray> headers{{"x-api-key", apiKey.trimmed().toUtf8()},
                                               {"anthropic-version", "2023-06-01"}};
    return fetchModelCatalog(url, timeoutMs, headers, errorOut, cancellationToken, outcome,
                             networkFactory, provider::parseAnthropicModelCatalog);
}

QList<OllamaModelSummary>
fetchOpenAiCloudModels(const QUrl& url, const QString& apiKey, int timeoutMs, QString* errorOut,
                       const std::shared_ptr<std::atomic_bool>& cancellationToken,
                       ProviderDiscoveryOutcome* outcome,
                       const std::shared_ptr<ILocalInferenceNetworkFactory>& networkFactory) {
    if (apiKey.trimmed().isEmpty()) {
        if (outcome)
            *outcome = {false, ChatProviderErrorCategory::AuthenticationRequired, 0, {}};
        if (errorOut) {
            *errorOut = QStringLiteral("API key is empty.");
        }
        return {};
    }
    if (url.scheme() != QLatin1String("https") || !LMStudioConfig::isSecureTransportEndpoint(url)) {
        if (outcome)
            *outcome = {false, ChatProviderErrorCategory::RequestRejected, 0, {}};
        if (errorOut)
            *errorOut = QStringLiteral("Cloud model discovery requires a valid HTTPS endpoint.");
        return {};
    }
    const QMap<QByteArray, QByteArray> headers{
        {"Authorization", ("Bearer " + apiKey.trimmed()).toUtf8()}};
    return fetchOpenAiCompatibleModels(url, timeoutMs, headers, errorOut, cancellationToken,
                                       outcome, networkFactory);
}
} // namespace sentinel::core
