// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/desktop/DaemonClient.h"

#include <QDir>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QUuid>
#include <cmath>

namespace sentinel::desktop {
Q_LOGGING_CATEGORY(desktopIpcLog, "sentinel.desktop.ipc")
namespace {
bool integer(const QJsonValue& value) {
    const double number = value.toDouble(-1);
    return value.isDouble() && std::isfinite(number) && number >= 0 && std::floor(number) == number;
}
bool validPayload(const QJsonObject& fields, const QJsonObject& payload) {
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const auto value = payload.value(it.key());
        const auto type = it.value().toString();
        if ((type == "string" && !value.isString()) || (type == "integer" && !integer(value)) ||
            (type == "boolean" && !value.isBool()) || (type == "array" && !value.isArray()) ||
            (type == "object" && !value.isObject())) {
            return false;
        }
    }
    return true;
}
} // namespace

QString DaemonClient::defaultSocketPath() {
#ifdef Q_OS_WIN
    // DaemonIpcServer resolves its relative endpoint name under the temporary
    // directory, so the client must resolve it the same way to reach the pipe.
    return QDir::tempPath() +
           QStringLiteral("/sentinel-daemon-%1").arg(qEnvironmentVariable("USERNAME"));
#else
    return QDir::homePath() + QStringLiteral("/.sentinel/run/daemon.sock");
#endif
}
DaemonClient::DaemonClient(QObject* parent)
    : DaemonClient(defaultSocketPath(), 10000, 1000, parent) {}
DaemonClient::DaemonClient(const QString& endpoint, int requestTimeoutMs, int reconnectMs,
                           QObject* parent)
    : QObject(parent), m_endpoint(endpoint), m_requestTimeoutMs(qMax(1, requestTimeoutMs)) {
    connect(&m_socket, &QLocalSocket::connected, this, &DaemonClient::onConnected);
    connect(&m_socket, &QLocalSocket::disconnected, this, &DaemonClient::onDisconnected);
    connect(&m_socket, &QLocalSocket::readyRead, this, &DaemonClient::onReadyRead);
    connect(&m_socket, &QLocalSocket::errorOccurred, this, [this] { onDisconnected(); });
    m_clock.start();
    m_connectionTimer.setSingleShot(true);
    m_connectionTimer.setInterval(m_requestTimeoutMs);
    connect(&m_connectionTimer, &QTimer::timeout, this, [this] {
        failPending(Error::RequestTimeout, QStringLiteral("request-timeout"));
        m_socket.abort();
        onDisconnected();
    });
    m_socket.setReadBufferSize(ipc::max_frame_bytes + 1);
    m_reconnectTimer.setInterval(qMax(1, reconnectMs));
    m_reconnectTimer.setSingleShot(true);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &DaemonClient::connectToDaemon);
    m_timeoutTimer.setInterval(qMin(100, m_requestTimeoutMs));
    connect(&m_timeoutTimer, &QTimer::timeout, this, [this] {
        const auto now = m_clock.elapsed();
        const auto ids = m_pending.keys();
        for (const auto& id : ids) {
            if (!m_pending.contains(id) || m_pending.value(id).deadline > now) {
                continue;
            }
            const bool handshake = id == m_helloId;
            m_pending.remove(id);
            fail(id, Error::RequestTimeout, QStringLiteral("request-timeout"));
            if (handshake) {
                m_socket.abort();
                onDisconnected();
            }
        }
    });
    m_timeoutTimer.start();
    QTimer::singleShot(0, this, &DaemonClient::connectToDaemon);
}
DaemonClient::~DaemonClient() {
    m_manualDisconnect = true;
    m_socket.disconnect(this);
    m_socket.abort();
}
bool DaemonClient::daemonReachable() const {
    return m_state == ConnectionState::Connected;
}
void DaemonClient::setState(ConnectionState state, const QString& summary) {
    const bool reachable = daemonReachable();
    const bool changed = m_state != state;
    m_state = state;
    m_statusSummary = summary;
    if (changed) {
        emit connectionStateChanged();
    }
    if (reachable != daemonReachable()) {
        emit daemonReachableChanged();
    }
    emit statusChanged();
}
void DaemonClient::connectToDaemon() {
    if (m_state == ConnectionState::VersionMismatch || m_state == ConnectionState::ShuttingDown ||
        m_socket.state() != QLocalSocket::UnconnectedState) {
        return;
    }
    m_manualDisconnect = false;
    m_reconnectTimer.stop();
    setState(m_everConnected ? ConnectionState::Reconnecting : ConnectionState::Connecting,
             m_everConnected ? tr("Reconnecting to daemon…") : tr("Connecting to daemon…"));
    m_connectionTimer.start();
    m_socket.connectToServer(m_endpoint);
}
void DaemonClient::disconnectFromDaemon() {
    m_manualDisconnect = true;
    m_connectionTimer.stop();
    m_reconnectTimer.stop();
    setState(ConnectionState::ShuttingDown, tr("Disconnecting from daemon…"));
    failPending(Error::RequestCancelled, QStringLiteral("request-cancelled"));
    m_socket.abort();
    m_input.clear();
    m_helloId.clear();
    setState(ConnectionState::Disconnected, tr("Daemon disconnected."));
}
void DaemonClient::onConnected() {
    ++m_epoch;
    emit connectionStateChanged();
    m_input.clear();
    m_helloId = request(Command::hello, {{"client_id", "sentinel-desktop"},
                                         {"major", ipc::major},
                                         {"minor", ipc::minor},
                                         {"capabilities", ipc::capabilities()}});
}
void DaemonClient::onDisconnected() {
    m_connectionTimer.stop();
    m_input.clear();
    m_helloId.clear();
    if (m_manualDisconnect || m_state == ConnectionState::VersionMismatch) {
        return;
    }
    setState(m_everConnected ? ConnectionState::Reconnecting : ConnectionState::Unavailable,
             m_everConnected ? tr("Daemon disconnected; reconnecting…")
                             : tr("Daemon unavailable."));
    if (m_lastError == Error::None) {
        m_lastError = Error::DaemonUnavailable;
        emit errorChanged();
    }
    failPending(Error::DaemonUnavailable, QStringLiteral("daemon-unavailable"));
    if (!m_reconnectTimer.isActive()) {
        m_reconnectTimer.start();
    }
}
QString DaemonClient::request(Command command, const QJsonObject& payload) {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto name = ipc::commandName(command);
    const auto fields = ipc::commands().value(name).toObject();
    QString code;
    Error error = Error::InvalidRequest;
    if (name.isEmpty() || !validPayload(fields, payload)) {
        code = QStringLiteral("invalid-payload");
    } else if (m_socket.state() != QLocalSocket::ConnectedState ||
               (command != Command::hello && !daemonReachable())) {
        code = QStringLiteral("daemon-unavailable");
        error = Error::DaemonUnavailable;
    } else if (command == Command::hello && !m_helloId.isEmpty()) {
        code = QStringLiteral("handshake-already-started");
    }
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        if (it.value().isString() && it.value().toString().size() > 65536) {
            code = QStringLiteral("invalid-payload");
        }
    }
    const QJsonObject envelope{
        {"version", QJsonObject{{"major", ipc::major}, {"minor", ipc::minor}}},
        {"type", "request"},
        {"id", id},
        {"name", name},
        {"payload", payload}};
    const auto frame = QJsonDocument(envelope).toJson(QJsonDocument::Compact) + '\n';
    if (frame.size() > ipc::max_frame_bytes ||
        m_socket.bytesToWrite() + frame.size() > ipc::max_pending_bytes ||
        m_pending.size() >= 256) {
        code = QStringLiteral("request-limit");
    }
    if (!code.isEmpty()) {
        QTimer::singleShot(0, this, [this, id, error, code] { fail(id, error, code); });
        return id;
    }
    m_pending.insert(id, {name, m_clock.elapsed() + m_requestTimeoutMs});
    if (command != Command::desktop_projection && command != Command::desktop_settings &&
        command != Command::session_list && command != Command::model_helper_state &&
        command != Command::agent_history)
        qCDebug(desktopIpcLog) << "request" << id << name << "generation" << m_serverGeneration;
    if (m_socket.write(frame) != frame.size()) {
        m_pending.remove(id);
        QTimer::singleShot(0, this, [this, id] {
            fail(id, Error::DaemonUnavailable, QStringLiteral("daemon-unavailable"));
        });
    }
    return id;
}
void DaemonClient::refresh() {
    if (daemonReachable()) {
        request(Command::daemon_status);
    } else {
        connectToDaemon();
    }
}
void DaemonClient::onReadyRead() {
    while (m_socket.bytesAvailable() > 0) {
        m_input += m_socket.read(qMin<qint64>(4096, m_socket.bytesAvailable()));
        qsizetype newline;
        while ((newline = m_input.indexOf('\n')) >= 0) {
            if (newline + 1 > ipc::max_frame_bytes) {
                protocolFailure();
                return;
            }
            const auto frame = m_input.left(newline);
            m_input.remove(0, newline + 1);
            QJsonParseError error;
            const auto doc = QJsonDocument::fromJson(frame, &error);
            if (error.error != QJsonParseError::NoError || !doc.isObject()) {
                protocolFailure();
                return;
            }
            consume(doc.object());
            if (m_socket.state() != QLocalSocket::ConnectedState) {
                return;
            }
        }
        if (m_input.size() >= ipc::max_frame_bytes) {
            protocolFailure();
            return;
        }
    }
}
void DaemonClient::consume(const QJsonObject& message) {
    const auto version = message.value("version").toObject();
    if (!integer(version.value("major")) || !integer(version.value("minor")) ||
        !message.value("type").isString() || !message.value("id").isString() ||
        !message.value("name").isString() || message.value("id").toString().size() > 128 ||
        !message.value("payload").isObject()) {
        protocolFailure();
        return;
    }
    if (version.value("major").toDouble() != ipc::major) {
        setState(ConnectionState::VersionMismatch, tr("Daemon protocol is incompatible."));
        failPending(Error::ProtocolMismatch, QStringLiteral("incompatible-major"));
        m_reconnectTimer.stop();
        m_socket.abort();
        return;
    }
    const auto type = message.value("type").toString();
    const auto id = message.value("id").toString();
    const auto name = message.value("name").toString();
    const auto payload = message.value("payload").toObject();
    if (type == "event") {
        if (!daemonReachable()) {
            protocolFailure();
            return;
        }
        if (!ipc::events().contains(name)) {
            if (version.value("minor").toDouble() <= ipc::minor) {
                protocolFailure();
            }
            return; // Compatible additive events only from future minor versions.
        }
        QJsonObject optional;
        const auto declared = ipc::optional_events().value(name).toObject();
        for (auto it = declared.begin(); it != declared.end(); ++it)
            if (payload.contains(it.key()))
                optional.insert(it.key(), it.value());
        if (!validPayload(ipc::events().value(name).toObject(), payload) ||
            !validPayload(optional, payload)) {
            protocolFailure();
            return;
        }
        emit eventReceived(name, payload);
        return;
    }
    if (type != "response" && type != "error") {
        protocolFailure();
        return;
    }
    if (!m_pending.contains(id)) {
        return; // Duplicate or expired correlation ID.
    }
    const auto pending = m_pending.value(id);
    if (type == "error") {
        if (!payload.value("code").isString()) {
            protocolFailure();
            return;
        }
        const auto code = payload.value("code").toString();
        const bool handshake = id == m_helloId;
        m_pending.remove(id);
        if (code == "incompatible-major") {
            setState(ConnectionState::VersionMismatch, tr("Daemon protocol is incompatible."));
            m_reconnectTimer.stop();
        }
        fail(id, mapError(code), code);
        if (handshake || code == "incompatible-major") {
            m_socket.abort();
            onDisconnected();
        }
        return;
    }
    if (name != pending.name || !ipc::responses().contains(name) ||
        !validPayload(ipc::responses().value(name).toObject(), payload)) {
        protocolFailure();
        return;
    }
    const auto optionalFields = ipc::optional_responses().value(name).toObject();
    for (auto it = optionalFields.begin(); it != optionalFields.end(); ++it) {
        if (payload.contains(it.key()) &&
            !validPayload(QJsonObject{{it.key(), it.value()}}, payload)) {
            protocolFailure();
            return;
        }
    }
    m_pending.remove(id);
    if (id == m_helloId) {
        if (payload.value("major").toDouble() != ipc::major) {
            setState(ConnectionState::VersionMismatch, tr("Daemon protocol is incompatible."));
            fail(id, Error::ProtocolMismatch, QStringLiteral("incompatible-major"));
            m_socket.abort();
            return;
        }
        m_connectionTimer.stop();
        m_lastError = Error::None;
        emit errorChanged();
        m_serverGeneration = payload.value("server_generation").toString();
        m_everConnected = true;
        setState(ConnectionState::Connected, tr("Daemon connected."));
        refresh();
    }
    if (name == "daemon.status") {
        m_statusSummary = payload.value("running").toBool() ? tr("Daemon connected.")
                                                            : tr("Daemon is shutting down.");
        emit statusChanged();
        emit statusReceived(payload);
    }
    emit responseReceived(id, name, payload);
}
void DaemonClient::protocolFailure() {
    failPending(Error::MalformedResponse, QStringLiteral("malformed-response"));
    fail({}, Error::MalformedResponse, QStringLiteral("malformed-response"));
    m_socket.abort();
    onDisconnected();
}
void DaemonClient::fail(const QString& id, Error error, const QString& code) {
    m_lastError = error;
    emit errorChanged();
    emit requestFailed(id, error, code);
}
void DaemonClient::failPending(Error error, const QString& code) {
    const auto ids = m_pending.keys();
    m_pending.clear();
    for (const auto& id : ids) {
        fail(id, error, code);
    }
}
DaemonClient::Error DaemonClient::mapError(const QString& code) {
    if (code == "incompatible-major") {
        return Error::ProtocolMismatch;
    }
    if (code == "unknown-session") {
        return Error::SessionNotFound;
    }
    if (code == "unknown-run") {
        return Error::RunNotFound;
    }
    if (code == "invalid-approval") {
        return Error::ApprovalExpired;
    }
    if (code == "permission-denied") {
        return Error::PermissionDenied;
    }
    if (code == "provider-unavailable") {
        return Error::ProviderUnavailable;
    }
    if (code == "model-unavailable") {
        return Error::ModelUnavailable;
    }
    if (code == "runtime-busy") {
        return Error::RuntimeBusy;
    }
    if (code == "runtime-unavailable" || code == "daemon-shutting-down") {
        return Error::DaemonUnavailable;
    }
    return Error::InternalDaemonError;
}
} // namespace sentinel::desktop
