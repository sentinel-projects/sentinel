// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/chat/ChatMessage.h"
#include "sentinel/core/voice/UnifiedAudioService.h"
#include "sentinel/desktop/DaemonClient.h"
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QTimer>
#include <QVariant>

namespace sentinel::desktop {
// Presentation cache only. All state and accepted transitions originate in the daemon.
class DesktopRuntimeClient final : public QObject {
    Q_OBJECT
public:
    explicit DesktopRuntimeClient(DaemonClient& transport, QObject* parent = nullptr);
    QString settingsValue(const QString& key, const QString& fallback) const {
        return m_settings.value(key).toString(fallback);
    }
    void setSetting(const QString& key, const QString& value);
    QVariant settingsService(const QString& action, const QVariantList& arguments, bool query);
    QVariant value(const QString& name) const;
    QVariant dispatch(const QString& name, const QVariantList& arguments);
    const QList<core::ChatMessage>& messages() const {
        return m_messages;
    }
    // Unlike transport/session readiness, this confirms the requested history
    // for the current session/run has arrived.
    bool historyLoaded() const {
        return m_historyLoaded;
    }
    DaemonClient& transport() const {
        return m_transport;
    }
    QString sessionId() const {
        return m_sessionId;
    }
    QString runId() const {
        return m_runId;
    }
    QJsonObject pendingApproval() const {
        return m_approval;
    }
    void setPreferredSession(const QString& id) {
        m_preferredSession = id;
    }
    QVariantMap quickPanelSnapshot() const;
    void refresh();
    void refreshExtensionsHub();
    bool performExtensionsHubAction(const QString& id, const QString& action);
    QVariantMap extensionsHubActionState() const;
    bool voiceAction(const QString& action);
    QVariantMap voiceState() const;
    bool respondToApproval(bool allow);
    bool respondToQuickApproval(bool allow);
    bool cancelRun();
    bool ready() const {
        return m_transport.daemonReachable() && m_loaded && m_settingsLoaded && m_sessionsLoaded &&
               !m_sessionId.isEmpty() && !m_attaching;
    }
signals:
    void changed();
    void sessionChanged(const QString& sessionId);
    void memoryManagementCompleted(const QVariantMap& result);
    void operationFailed(const QString& code);
    void conversationDeleteCompleted(const QString& conversationId, bool succeeded, const QString& summary);

private:
    struct Request {
        QString name;
        QString target;
        QString runId;
        QString conversationId;
        QString memoryTag;
    };
    QString send(DaemonClient::Command command, const QJsonObject& payload = {},
                 const QString& target = {});
    void onResponse(const QString& id, const QString& name, const QJsonObject& payload);
    void onEvent(const QString& name, const QJsonObject& payload);
    void attach(const QString& id);
    void clearSession();
    void applySnapshot(const QJsonObject& snapshot);
    void requestMessages();
    void projectRun();
    void sendVoiceChunk();
    DaemonClient& m_transport;
    QHash<QString, Request> m_requests;
    QHash<QString, QVariant> m_values;
    QHash<QString, QVariant> m_projection;
    QList<core::ChatMessage> m_messages;
    QJsonArray m_sessions;
    QJsonObject m_approval;
    QJsonObject m_settings;
    QHash<QString, QVariantMap> m_settingsStates;
    QSet<QString> m_settingsQueries;
    QString m_sessionId, m_preferredSession, m_runId, m_state, m_kind, m_output, m_generation;
    QString m_submissionError, m_submissionErrorSummary;
    QString m_extensionActionPending;
    QVariantMap m_extensionActionState;
    quint64 m_sequence = 0;
    QTimer m_refreshTimer;
    QTimer m_voiceTimer;
    QTimer m_captureLimit;
    core::AudioDeviceService m_voiceDevices;
    QByteArray m_outboundVoicePcm;
    QByteArray m_capturedVoiceSegments;
    qsizetype m_voiceOffset = 0;
    bool m_voiceSpeechDetected = false;
    bool m_voiceStartPending = false;
    bool m_microphonePermissionPending = false;
    QString m_localVoiceFailure;
    quint64 m_voiceGeneration = 0;
    QVariantMap m_voiceState;
    bool m_voiceQueryPending = false;
    bool m_historyLoaded = false;
    bool m_settingsLoaded = false, m_sessionsLoaded = false;
    bool m_loaded = false, m_attaching = false, m_submissionPending = false;
    QElapsedTimer m_startupProjectionTimer;
    int m_projectionRemaining = 0;
    int m_activeAssistantId = 0;
    bool m_approvalResponsePending = false;
};
} // namespace sentinel::desktop
