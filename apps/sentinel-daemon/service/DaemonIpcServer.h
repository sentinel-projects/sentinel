// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#if defined(Q_OS_UNIX)
#include <sys/types.h>
#endif
namespace sentinel::core {
class ApplicationController;
class AppSettings;
struct AgentEvent;
struct ToolExecutionResult;
} // namespace sentinel::core
namespace sentinel::daemon {
class DaemonModelHelpers;
class DaemonIpcServer final : public QObject {
    Q_OBJECT
public:
    explicit DaemonIpcServer(core::ApplicationController* controller = nullptr,
                             QObject* parent = nullptr);
    ~DaemonIpcServer() override;
    static QString defaultSocketPath();
    bool startServer(const QString& serverName = {});
    // Typed diagnostic for the most recent startServer failure. Contains sizes
    // and platform limits, never the configured path itself.
    QString lastServerError() const {
        return m_lastServerError;
    }
    void stopServer();
    void requestShutdown();
    void setController(core::ApplicationController* controller);
    bool openSessionProjectionStore(const QString& path);
    void setSettings(core::AppSettings* settings) {
        m_settings = settings;
    }
#if defined(Q_OS_UNIX)
    using PeerUidResolver = std::function<std::optional<uid_t>(qintptr)>;
    static std::optional<uid_t> querySocketPeerUid(qintptr descriptor);
    // Safely constrained test-only hook for deterministic peer credential simulation.
    // Cannot be altered while the IPC listener is active.
    void setPeerUidResolverForTesting(PeerUidResolver resolver) {
        if (m_server.isListening()) {
            return;
        }
        m_peerUidResolver = std::move(resolver);
    }
#endif

signals:
    void shutdownStarted();
    void shutdownDrained();

private:
    QTimer m_shutdownDrain;
    QPointer<QLocalSocket> m_voiceOwner;
    QByteArray m_voicePcm;
    bool m_voiceCaptureReserved = false;
    quint64 m_voiceSttRevision = 0;
    QString m_lastServerError;
    struct BackupTransfer {
        QString id;
        QByteArray bytes;
        QByteArray sha256;
        QStringList domains;
        int expected = 0;
        bool importing = false;
        bool replace = false;
        QDateTime touched;
    };
    QHash<QLocalSocket*, BackupTransfer> m_backupTransfers;
    struct Client {
        QByteArray input;
        bool hello = false;
        QSet<QString> sessions;
        // Freeze each catalogue while its bounded pages are being transferred.
        QHash<QString, QJsonObject> modelSnapshots;
        QHash<QString, QJsonArray> completedModelRows;
    };
    void handleNewConnection();
    void handleRequest(const QJsonObject& message, QLocalSocket* socket);
    void send(QLocalSocket* socket, const QJsonObject& message);
    void error(QLocalSocket* socket, const QString& id, const QString& code);
    void publishEvent(const QString& name, const QJsonObject& payload = {});
    void onChatChanged();
    void scheduleConversationTitle();
    QPointer<QThread> m_titleWorker;
    std::shared_ptr<std::atomic_bool> m_titleCancellation;
    QHash<QString, int> m_titleTurns;
    QSet<QString> m_generatedTitles;
    void onAgentEvent(const core::AgentEvent& event);
    QJsonObject session(const QString& id, bool presentation = false) const;
    void rememberRun();
    struct ChangeBaseline {
        QString root;
        QString workspaceId;
        QHash<QString, QString> contents;
        QSet<QString> listed;
        bool complete = false;
        bool truncated = false;
    };
    core::ToolExecutionResult inspectFile(const QString& sessionId, const QString& root,
                                          const QString& tool, const QString& path);
    void captureChangeBaseline(const QString& sessionId);
    QJsonObject reviewChanges(const QString& sessionId);
    QHash<QString, ChangeBaseline> m_changeBaselines;
    QString m_projectionConnection;
    QHash<QString, QJsonObject> m_lastRuns;
    QLocalServer m_server;
    core::ApplicationController* m_controller = nullptr;
    core::AppSettings* m_settings = nullptr;
    QHash<QLocalSocket*, Client> m_clients;
    std::unique_ptr<QLockFile> m_lock;
    QString m_path;
    QString m_generation;
    quint64 m_eventSequence = 0;
    QString m_agentSubscription;
    QString m_sessionId, m_runId, m_kind, m_approvalId, m_state, m_output;
    QString m_providerId, m_modelId;
    QJsonObject m_approvalPayload;
    QString m_agentTurnId;
    QSet<QString> m_subagentSessions;
    QHash<QString, QDateTime> m_toolStarts;
    bool m_outputTruncated = false;
    int m_chatCursor = 0;
    bool m_starting = false;
    bool m_loadingAttachments = false;
    bool m_shuttingDown = false;
    QElapsedTimer m_uptime;
    std::unique_ptr<DaemonModelHelpers> m_modelHelpers;
#if defined(Q_OS_UNIX)
    PeerUidResolver m_peerUidResolver;
#endif
};
} // namespace sentinel::daemon
