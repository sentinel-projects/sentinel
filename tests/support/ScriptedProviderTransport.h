// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/runtime/LocalInferenceTransport.h"

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <cstring>
#include <functional>
#include <memory>
#include <utility>

namespace sentinel::test {

// One observed provider POST as it was actually constructed beneath the native
// request builder. Fixtures assert against these fields instead of a real socket.
struct RecordedProviderRequest {
    QUrl url;
    QByteArray method;
    QMap<QByteArray, QByteArray> headers;
    QByteArray body;
    int redirectPolicy = -1;
};

// Deterministic synthetic HTTP response. `frames` are emitted through readyRead
// in order (so streaming parsers see real incremental frames); `finish` false
// holds the reply open so the caller's deadline or cancellation must abort it.
struct ScriptedHttpResponse {
    int status = 200;
    QList<QByteArray> frames;
    QNetworkReply::NetworkError error = QNetworkReply::NoError;
    QString errorString;
    QMap<QByteArray, QByteArray> rawHeaders;
    bool finish = true;
    int frameDelayMs = 0;
    bool emitConnectionSignals = false;
    bool emitRequestSentSignal = false;
    bool emitMetadataSignal = false;
    bool immediateFinish = false;

    static ScriptedHttpResponse body(const QByteArray& payload, int status = 200) {
        ScriptedHttpResponse response;
        response.status = status;
        response.frames = {payload};
        return response;
    }
    static ScriptedHttpResponse streaming(const QList<QByteArray>& chunks, bool complete = true) {
        ScriptedHttpResponse response;
        response.frames = chunks;
        response.finish = complete;
        return response;
    }
};

class ScriptedNetworkReply final : public QNetworkReply {
public:
    ScriptedNetworkReply(const QNetworkRequest& request, ScriptedHttpResponse response,
                         QObject* parent, QNetworkAccessManager::Operation operation)
        : QNetworkReply(parent), response_(std::move(response)) {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        if (response_.status > 0)
            setAttribute(QNetworkRequest::HttpStatusCodeAttribute, response_.status);
        for (auto it = response_.rawHeaders.constBegin(); it != response_.rawHeaders.constEnd();
             ++it) {
            setRawHeader(it.key(), it.value());
        }
        if (response_.error != QNetworkReply::NoError) {
            setError(response_.error, response_.errorString);
        }
        open(QIODevice::ReadOnly);
        timer_.setSingleShot(true);
        timer_.setInterval(response_.frameDelayMs);
        connect(&timer_, &QTimer::timeout, this, [this] { deliverNext(); });
        if (response_.immediateFinish)
            deliverNext();
        else
            timer_.start();
    }

    void abort() override {
        if (finished_ || aborted_) {
            return;
        }
        aborted_ = true;
        timer_.stop();
        setError(QNetworkReply::OperationCanceledError, QStringLiteral("Request cancelled."));
        finished_ = true;
        setFinished(true);
        emit finished();
    }

    qint64 readData(char* data, qint64 maxSize) override {
        if (maxSize <= 0) {
            return 0;
        }
        const qint64 available = static_cast<qint64>(buffer_.size()) - offset_;
        if (available <= 0) {
            return 0;
        }
        const qint64 count = qMin(maxSize, available);
        std::memcpy(data, buffer_.constData() + offset_, static_cast<size_t>(count));
        offset_ += count;
        return count;
    }

    qint64 bytesAvailable() const override {
        return (static_cast<qint64>(buffer_.size()) - offset_) + QNetworkReply::bytesAvailable();
    }

private:
    void deliverNext() {
        if (finished_ || aborted_) {
            return;
        }
        if (!signalsDelivered_) {
            signalsDelivered_ = true;
            if (response_.emitConnectionSignals) {
                emit socketStartedConnecting();
                emit encrypted();
            }
            if (response_.emitRequestSentSignal)
                emit requestSent();
            if (response_.emitMetadataSignal)
                emit metaDataChanged();
            if (response_.error != QNetworkReply::NoError)
                emit errorOccurred(response_.error);
        }
        if (frameIndex_ < response_.frames.size()) {
            buffer_.append(response_.frames.at(frameIndex_++));
            emit readyRead();
            if (frameIndex_ < response_.frames.size()) {
                timer_.start();
                return;
            }
        }
        if (response_.finish) {
            finished_ = true;
            setFinished(true);
            emit finished();
        }
        // finish == false holds the reply open; only the caller's deadline or
        // cancellation reaches abort().
    }

    ScriptedHttpResponse response_;
    QTimer timer_;
    int frameIndex_ = 0;
    QByteArray buffer_;
    qint64 offset_ = 0;
    bool finished_ = false;
    bool aborted_ = false;
    bool signalsDelivered_ = false;
};

struct ScriptedTransportState {
    QList<RecordedProviderRequest> requests;
    std::function<ScriptedHttpResponse(const RecordedProviderRequest&)> responder;
    std::function<void()> onCreateManager;
};

class ScriptedNetworkAccessManager final : public QNetworkAccessManager {
public:
    explicit ScriptedNetworkAccessManager(std::shared_ptr<ScriptedTransportState> state)
        : state_(std::move(state)) {}

protected:
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice* outgoingData) override {
        QByteArray body;
        if (outgoingData) {
            body = outgoingData->readAll();
        }
        RecordedProviderRequest record;
        record.url = request.url();
        record.method = op == PostOperation  ? QByteArrayLiteral("POST")
                        : op == GetOperation ? QByteArrayLiteral("GET")
                                             : QByteArrayLiteral("OTHER");
        const auto headerNames = request.rawHeaderList();
        for (const auto& name : headerNames) {
            record.headers.insert(name.toLower(), request.rawHeader(name));
        }
        record.body = body;
        record.redirectPolicy = request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt();
        state_->requests.append(record);
        const auto response =
            state_->responder ? state_->responder(record) : ScriptedHttpResponse{};
        auto* reply = new ScriptedNetworkReply(request, response, this, op);
        if (outgoingData) {
            outgoingData->setParent(reply);
        }
        return reply;
    }

private:
    std::shared_ptr<ScriptedTransportState> state_;
};

class ScriptedNetworkFactory final : public core::ILocalInferenceNetworkFactory {
public:
    explicit ScriptedNetworkFactory(std::shared_ptr<ScriptedTransportState> state)
        : state_(std::move(state)) {}
    std::unique_ptr<QNetworkAccessManager> createManager() const override {
        if (state_->onCreateManager)
            state_->onCreateManager();
        return std::make_unique<ScriptedNetworkAccessManager>(state_);
    }
    std::shared_ptr<ScriptedTransportState> state_;
};

// Temporarily allow non-loopback hosts so the real endpoint/host validation and
// NetworkPolicyService gate are exercised for the production host names. The
// injected transport never opens a socket, so no real traffic is generated.
class ScopedOnlineNetworkMode {
public:
    ScopedOnlineNetworkMode() : previous_(core::NetworkPolicyService::instance().mode()) {
        core::NetworkPolicyService::instance().setMode(core::NetworkMode::Online);
    }
    ~ScopedOnlineNetworkMode() {
        core::NetworkPolicyService::instance().setMode(previous_);
    }
    ScopedOnlineNetworkMode(const ScopedOnlineNetworkMode&) = delete;
    ScopedOnlineNetworkMode& operator=(const ScopedOnlineNetworkMode&) = delete;

private:
    core::NetworkMode previous_;
};

} // namespace sentinel::test
