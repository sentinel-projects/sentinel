// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QJsonDocument>
#include <QPermissions>
#include <cmath>
#include <algorithm>
#include "sentinel/core/extension/ExtensionService.h"

namespace sentinel::desktop {
namespace {
const QJsonObject& projectionFields() {
    static const auto fields = ipc::desktopProjectionFields();
    return fields;
}
core::ChatMessageStatus messageStatus(const QString& state) {
    if (state == "streaming" || state == "running") {
        return core::ChatMessageStatus::Streaming;
    }
    if (state == "cancelled") {
        return core::ChatMessageStatus::Cancelled;
    }
    if (state == "failed" || state == "error") {
        return core::ChatMessageStatus::Failed;
    }
    if (state == "interrupted") {
        return core::ChatMessageStatus::Interrupted;
    }
    if (state == "queued") {
        return core::ChatMessageStatus::Queued;
    }
    if (state == "sent") {
        return core::ChatMessageStatus::Sent;
    }
    return core::ChatMessageStatus::Completed;
}
bool active(const QString& state) {
    return state == "running" || state == "approval";
}
bool validProperties(const QJsonObject& properties) {
    const auto& fields = projectionFields();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!fields.contains(it.key())) {
            return false;
        }
        const auto field = fields.value(it.key()).toObject();
        const auto type = field.value("type").toString();
        const auto value = it.value();
        if ((type == "string" && !value.isString()) || (type == "boolean" && !value.isBool()) ||
            (type == "array" && !value.isArray()) || (type == "object" && !value.isObject()) ||
            ((type == "integer" || type == "number") &&
             (!value.isDouble() || !std::isfinite(value.toDouble()))) ||
            (type == "integer" && std::floor(value.toDouble()) != value.toDouble())) {
            return false;
        }
        if (field.value("cpp") == "QStringList") {
            for (const auto& item : value.toArray()) {
                if (!item.isString()) {
                    return false;
                }
            }
        }
    }
    return true;
}
} // namespace
DesktopRuntimeClient::DesktopRuntimeClient(DaemonClient& transport, QObject* parent)
    : QObject(parent), m_transport(transport) {
    connect(&transport, &DaemonClient::responseReceived, this, &DesktopRuntimeClient::onResponse);
    connect(&transport, &DaemonClient::eventReceived, this, &DesktopRuntimeClient::onEvent);
    connect(&transport, &DaemonClient::requestFailed, this,
            [this](const QString& id, DaemonClient::Error, const QString& code) {
                if (!m_requests.contains(id)) {
                    return;
                }
                const auto failed = m_requests.take(id);
                if (failed.target == "manageMemory") {
                    emit memoryManagementCompleted(
                        {{"status", "Unavailable"}, {"requestTag", failed.memoryTag}});
                }
                if (id == m_extensionActionPending) {
                    m_extensionActionPending.clear();
                    m_extensionActionState["phase"] = "unconfirmed";
                    refreshExtensionsHub();
                    return;
                }
                if (failed.name == "desktop.settings_service" &&
                    failed.target.startsWith("extensionsHubState[")) {
                    m_settingsStates[failed.target] = QVariantMap{{"code", "ExtensionInventoryUnavailable"}};
                    m_settingsQueries.remove(failed.target);
                    emit changed();
                    return;
                }
                if (failed.target == "requestPermanentDeleteConversation")
                    emit conversationDeleteCompleted(failed.conversationId, false,
                        code == "request-timeout"
                            ? tr("Deletion could not be confirmed. Refresh the conversation list before retrying.")
                            : code == "runtime-busy"
                            ? tr("Stop the active response before deleting this conversation.")
                            : tr("The conversation could not be deleted: %1").arg(code));
                const bool generationRequest = failed.name == "chat.send" || failed.name == "agent.start" ||
                                               failed.name == "chat.retry" || failed.name == "chat.regenerate" ||
                                               failed.name == "chat.edit";
                if (generationRequest) {
                    m_submissionError = code;
                    m_submissionErrorSummary =
                        code == "provider-unavailable"
                            ? tr("The selected provider is unavailable. Check that its server is running "
                                 "and its endpoint is correct in Settings.")
                        : code == "model-unavailable"
                            ? tr("The selected model is unavailable. Load the model in its server "
                                 "and select an available model.")
                        : code == "runtime-busy"
                            ? tr("Another request is still running. Wait for it or stop it before retrying.")
                            : tr("The message could not be sent: %1").arg(code);
                }
                if (failed.name == "voice.state") {
                    m_voiceQueryPending = false;
                }
                if (failed.name == "desktop.projection") {
                    m_projectionRemaining = 0;
                }
                if (failed.name == "desktop.settings_service") {
                    m_settingsStates[failed.target] = QVariantMap{{"accepted", false}, {"code", code}};
                    if (!failed.target.startsWith("productSettings[") &&
                        !failed.target.startsWith("extensionSettingsState["))
                        m_settingsQueries.remove(failed.target);
                }
                if (failed.name == "voice.action" || failed.name == "voice.audio") {
                    ++m_voiceGeneration;
                    m_voiceStartPending = false;
                    m_microphonePermissionPending = false;
                    m_captureLimit.stop();
                    m_voiceDevices.cancelCapture();
                    m_outboundVoicePcm.clear();
                    m_localVoiceFailure = code;
                }
                m_submissionPending = false;
                if (failed.name == "approval.respond") {
                    m_approvalResponsePending = false;
                }
                m_attaching = false;
                m_values["chatErrorCategory"] = code;
                m_values["chatSendLifecycleSummary"] = code;
                emit operationFailed(code);
                projectRun();
                emit changed();
            });
    connect(&transport, &DaemonClient::connectionStateChanged, this, [this] {
        if (!m_transport.daemonReachable()) {
            ++m_voiceGeneration;
            m_microphonePermissionPending = false;
            m_voiceStartPending = false;
            m_captureLimit.stop();
            m_voiceDevices.cancelCapture();
            m_outboundVoicePcm.clear();
            m_capturedVoiceSegments.clear();
            m_voiceState.clear();
            m_voiceQueryPending = false;
            m_loaded = false;
            m_settingsLoaded = false;
            m_sessionsLoaded = false;
            m_projection.clear();
            m_settings = {};
            m_values.clear();
            m_messages.clear();
            m_historyLoaded = false;
            m_requests.clear();
            m_extensionActionPending.clear();
            m_extensionActionState.clear();
            m_settingsStates.remove("extensionsHubState[]");
            m_settingsQueries.remove("extensionsHubState[]");
            m_submissionPending = false;
            m_attaching = false;
            m_projectionRemaining = 0;
            m_approval = {};
            m_approvalResponsePending = false;
            m_activeAssistantId = 0;
            projectRun();
            emit changed();
            return;
        }
        m_startupProjectionTimer.start();
        if (m_generation != m_transport.serverGeneration()) {
            m_sequence = 0;
            m_runId.clear();
            m_state.clear();
            m_kind.clear();
            m_values.clear();
            m_settingsStates.clear();
            m_settingsQueries.clear();
            m_settings = {};
            m_generation = m_transport.serverGeneration();
        }
        refresh();
    });
    m_refreshTimer.setInterval(2000);
    connect(&m_refreshTimer, &QTimer::timeout, this, [this] {
        if (m_transport.daemonReachable() && m_projectionRemaining == 0) {
            m_projectionRemaining = -1;
            send(DaemonClient::Command::desktop_projection, {{"page", 0}});
            send(DaemonClient::Command::session_list);
            send(DaemonClient::Command::desktop_settings);
            m_settingsQueries.clear();
        }
    });
    m_refreshTimer.start();
    m_voiceTimer.setInterval(2000);
    connect(&m_voiceTimer, &QTimer::timeout, this, [this] {
        if (ready() && !m_voiceQueryPending) {
            m_voiceQueryPending = true;
            send(DaemonClient::Command::voice_state);
        }
    });
    m_voiceTimer.start();
    connect(&m_voiceDevices, &core::AudioDeviceService::speechSegmentReady, this,
            [this](const QByteArray& pcm) {
                if (m_capturedVoiceSegments.size() + pcm.size() > 16000LL * 2 * 60) {
                    voiceAction("cancel");
                    m_localVoiceFailure = "CaptureFailure";
                } else {
                    m_capturedVoiceSegments.append(pcm);
                }
            });
    m_captureLimit.setSingleShot(true);
    m_captureLimit.setInterval(59000);
    connect(&m_captureLimit, &QTimer::timeout, this, [this] { voiceAction("stop"); });
    connect(&m_voiceDevices, &core::AudioDeviceService::captureFailed, this,
            [this](core::AudioFailure failure) {
                voiceAction("cancel");
                m_localVoiceFailure = core::audioFailureName(failure);
                emit changed();
            });
}
QVariantMap DesktopRuntimeClient::voiceState() const {
    auto result = m_voiceState;
    result["level"] = m_voiceDevices.inputLevel();
    result["requesting_permission"] = m_microphonePermissionPending;
    if (m_microphonePermissionPending || m_voiceStartPending) {
        result["available"] = false;
    }
    if (m_microphonePermissionPending) {
        result["state"] = "RequestingPermission";
    } else if (m_voiceStartPending) {
        result["state"] = "StartingCapture";
    } else if (m_voiceDevices.isCapturing()) {
        result["state"] = "Listening";
    } else if (!m_outboundVoicePcm.isEmpty()) {
        result["state"] = "Transcribing";
    }
    if (!m_localVoiceFailure.isEmpty()) {
        result["failure"] = m_localVoiceFailure;
        result["state"] = "Failed";
    }
    return result;
}
void DesktopRuntimeClient::sendVoiceChunk() {
    constexpr qsizetype chunkSize = 49152;
    const auto chunk = m_outboundVoicePcm.mid(m_voiceOffset, chunkSize);
    m_voiceOffset += chunk.size();
    send(DaemonClient::Command::voice_audio,
         {{"pcm", QString::fromLatin1(chunk.toBase64())},
          {"final", m_voiceOffset == m_outboundVoicePcm.size()},
          {"speech", m_voiceSpeechDetected}},
         QString::number(m_voiceGeneration));
}
bool DesktopRuntimeClient::voiceAction(const QString& action) {
    if (!ready()) {
        return false;
    }
    if (action == "cancel") {
        ++m_voiceGeneration;
        m_microphonePermissionPending = false;
        m_voiceStartPending = false;
        m_captureLimit.stop();
        m_voiceDevices.cancelCapture();
        m_capturedVoiceSegments.clear();
        m_outboundVoicePcm.clear();
        m_voiceState["transcript"] = QString{};
        send(DaemonClient::Command::voice_action, {{"action", "cancel"}}, "cancel");
    } else if (action == "stop") {
        if (!m_voiceDevices.isCapturing()) {
            return false;
        }
        m_captureLimit.stop();
        const auto finalSegment = m_voiceDevices.stopCapture();
        m_outboundVoicePcm = std::move(m_capturedVoiceSegments);
        m_outboundVoicePcm.append(finalSegment);
        m_voiceSpeechDetected = m_voiceDevices.speechDetected();
        m_voiceOffset = 0;
        if (m_outboundVoicePcm.isEmpty() || m_outboundVoicePcm.size() > 16000LL * 2 * 60) {
            voiceAction("cancel");
            m_localVoiceFailure = "CaptureFailure";
        } else {
            sendVoiceChunk();
        }
    } else if (action == "start") {
        if (active(m_state) || m_microphonePermissionPending || m_voiceStartPending ||
            m_voiceDevices.isCapturing() || !m_outboundVoicePcm.isEmpty() ||
            !m_voiceState.value("available").toBool() ||
            m_voiceState.value("state").toString() == "Transcribing") {
            return false;
        }
        m_localVoiceFailure.clear();
        m_capturedVoiceSegments.clear();
        m_voiceState["transcript"] = QString{};
        const auto generation = ++m_voiceGeneration;
        const auto begin = [this, generation] {
            if (generation != m_voiceGeneration || !ready()) {
                return;
            }
            m_microphonePermissionPending = false;
            if (QCoreApplication::instance()->checkPermission(QMicrophonePermission{}) !=
                Qt::PermissionStatus::Granted) {
                m_localVoiceFailure = "MicrophonePermissionDenied";
                emit changed();
                return;
            }
            m_voiceStartPending = true;
            send(DaemonClient::Command::voice_action, {{"action", "start"}},
                 QString::number(generation));
            emit changed();
        };
        if (QCoreApplication::instance()->checkPermission(QMicrophonePermission{}) ==
            Qt::PermissionStatus::Undetermined) {
            m_microphonePermissionPending = true;
            QCoreApplication::instance()->requestPermission(
                QMicrophonePermission{}, this, [begin](const QPermission&) { begin(); });
        } else {
            begin();
        }
    } else {
        return false;
    }
    m_voiceTimer.setInterval(100);
    emit changed();
    return true;
}
QString DesktopRuntimeClient::send(DaemonClient::Command command, const QJsonObject& payload,
                                   const QString& target) {
    const auto id = m_transport.request(command, payload);
    m_requests.insert(id, {ipc::commandName(command), target, m_runId, {}});
    return id;
}
void DesktopRuntimeClient::refresh() {
    if (!m_transport.daemonReachable()) {
        return;
    }
    if (m_projectionRemaining == 0) {
        m_projectionRemaining = -1;
        send(DaemonClient::Command::desktop_projection, {{"page", 0}});
    }
    send(DaemonClient::Command::session_list);
    send(DaemonClient::Command::desktop_settings);
    if (!m_sessionId.isEmpty()) {
        attach(m_sessionId);
    }
}
void DesktopRuntimeClient::refreshExtensionsHub() {
    auto extensionQuery = [](const QString& key) {
        return key.startsWith("extensionsHubState[") || key.startsWith("productSettings[") ||
               key.startsWith("extensionSettingsState[");
    };
    // Invalidate pending reads as well as cached values. A pre-refresh response cannot repopulate them.
    for (auto it = m_requests.begin(); it != m_requests.end();) {
        if (it->name == "desktop.settings_service" && extensionQuery(it->target)) {
            it = m_requests.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& key : m_settingsQueries.values()) {
        if (extensionQuery(key)) {
            m_settingsQueries.remove(key);
        }
    }
    for (const auto& key : m_settingsStates.keys()) {
        if (extensionQuery(key)) {
            m_settingsStates.remove(key);
        }
    }
    emit changed();
}
QVariantMap DesktopRuntimeClient::extensionsHubActionState() const {
    return ready() ? m_extensionActionState : QVariantMap{{"phase", "disconnected"}};
}
bool DesktopRuntimeClient::performExtensionsHubAction(const QString& id, const QString& action) {
    if (!ready() || !m_extensionActionPending.isEmpty() ||
        (action != "disable" && action != "disconnect")) {
        return false;
    }
    bool allowed = false;
    for (const auto& value : m_settingsStates.value("extensionsHubState[]").value("entries").toList()) {
        const auto row = value.toMap();
        if (row.value("id").toString() == id && row.value("actions").toStringList().contains(action)) {
            allowed = true;
            break;
        }
    }
    if (!allowed) {
        return false;
    }
    const int operation = static_cast<int>(action == "disable" ? core::ExtensionAction::Disable
                                                               : core::ExtensionAction::Disconnect);
    m_extensionActionState = {{"id", id}, {"action", action}, {"phase", "pending"}};
    m_extensionActionPending = send(DaemonClient::Command::desktop_settings_service,
        {{"action", "performExtensionSettingsAction"}, {"arguments", QJsonArray{id, operation}}},
        "performExtensionSettingsAction[]");
    emit changed();
    return true;
}
QVariant DesktopRuntimeClient::settingsService(const QString& action, const QVariantList& arguments,
                                               bool query) {
    if (!ipc::desktopSettingsActions().contains(action)) {
        return QVariantMap{{"accepted", false}, {"code", "UnknownSettingsAction"}};
    }
    const auto serialized =
        QJsonDocument(QJsonArray::fromVariantList(arguments)).toJson(QJsonDocument::Compact);
    const auto key = action + (query ? QString::fromUtf8(serialized) : QStringLiteral("[]"));
    if (!ready()) {
        return QVariantMap{{"accepted", false}, {"code", "DaemonUnavailable"}};
    }
    if (query) {
        const bool pending = std::any_of(m_requests.cbegin(), m_requests.cend(), [&](const auto& request) {
            return request.name == "desktop.settings_service" && request.target == key;
        });
        if (!m_settingsQueries.contains(key) && !pending) {
            m_settingsQueries.insert(key);
            send(DaemonClient::Command::desktop_settings_service,
                 {{"action", action}, {"arguments", QJsonArray::fromVariantList(arguments)}}, key);
        }
        return m_settingsStates.value(key);
    }
    const auto id =
        send(DaemonClient::Command::desktop_settings_service,
             {{"action", action}, {"arguments", QJsonArray::fromVariantList(arguments)}}, key);
    return QVariantMap{{"accepted", false}, {"code", "Pending"}, {"requestId", id}};
}
void DesktopRuntimeClient::setSetting(const QString& key, const QString& value) {
    if (!ready()) {
        emit operationFailed(QStringLiteral("daemon-unavailable"));
        return;
    }
    send(DaemonClient::Command::desktop_setting, {{"key", key}, {"value", value}});
}
QVariant DesktopRuntimeClient::value(const QString& name) const {
    if (name == "localChatSendAvailable") {
        return ready() && !m_sessionId.isEmpty() && !active(m_state) && !m_submissionPending &&
               m_values.value(name).toBool();
    }
    if (name == "providerStatus" && !m_transport.daemonReachable()) {
        return m_transport.statusSummary();
    }
    if (name == "localChatSendAvailabilitySummary" && !ready()) {
        return m_transport.statusSummary();
    }
    return m_values.value(name);
}
QVariant DesktopRuntimeClient::dispatch(const QString& name, const QVariantList& args) {
    if (args.isEmpty() && m_values.contains(name)) {
        return value(name);
    }
    // Snapshot readers are allowed before the first projection arrives.
    if (args.isEmpty() && projectionFields().contains(name)) {
        return value(name);
    }
    if (name == "attachControlledTaskSettings") {
        return {};
    }
    if (!ready()) {
        return false;
    }
    const auto text = args.isEmpty() ? QString{} : args.first().toString();
    if (name == "createConversation") {
        return send(DaemonClient::Command::session_create, {{"title", text}});
    }
    if (name == "switchConversation") {
        attach(text);
        return true;
    }
    if (name == "sendMessage" || name == "sendMessageWithAttachments" || name == "runAgentRequestWithAttachments" || name == "runAgentRequest" || name == "runLocalInference") {
        if (m_sessionId.isEmpty() || active(m_state) || m_submissionPending ||
            text.trimmed().isEmpty()) {
            return false;
        }
        if (name == "runLocalInference" && args.size() > 1 && !args.at(1).toString().isEmpty() &&
            args.at(1).toString() != value("selectedLocalModel").toString()) {
            return false;
        }
        m_submissionPending = true;
        m_submissionError.clear();
        m_submissionErrorSummary.clear();
        m_values["chatErrorCategory"] = QString{};
        m_values["chatSendLifecycleSummary"] = QString{};
        m_output.clear();
        m_approval = {};
        m_values.remove("latestToolExecutionSummary");
        m_values.remove("latestAgentActivitySummary");
        m_activeAssistantId = 0;
        m_kind = name.startsWith("runAgentRequest") ? "agent" : "chat";
        send(m_kind == "agent" ? DaemonClient::Command::agent_start
                               : DaemonClient::Command::chat_send,
             {{"session_id", m_sessionId}, {"text", text},
              {"attachments", name.endsWith("WithAttachments") && args.size() > 1 ? QJsonArray::fromVariantList(args.at(1).toList()) : QJsonArray{}}}, m_sessionId);
        projectRun();
        emit changed();
        return true;
    }
    if (name == "stopChatGeneration" || name == "cancelAgentRun" ||
        name == "cancelLocalInference") {
        return cancelRun();
    }
    if (name == "respondToAgentApproval") {
        return respondToApproval(args.first().toBool());
    }
    if (name == "regenerateChatResponse" || name == "retryChatResponse" ||
        name == "editAndResendChatMessage") {
        if (active(m_state) || m_submissionPending || args.isEmpty()) {
            return false;
        }
        QJsonObject payload{{"session_id", m_sessionId}, {"message_id", args.first().toInt()}};
        auto command = name == "retryChatResponse" ? DaemonClient::Command::chat_retry
                                                   : DaemonClient::Command::chat_regenerate;
        if (name == "editAndResendChatMessage") {
            command = DaemonClient::Command::chat_edit;
            payload.insert("text", args.at(1).toString());
        }
        m_submissionPending = true;
        m_kind = "chat";
        m_output.clear();
        m_activeAssistantId = 0;
        m_submissionError.clear();
        m_submissionErrorSummary.clear();
        m_values["chatErrorCategory"] = QString{};
        m_values["chatSendLifecycleSummary"] = QString{};
        const auto id = send(command, payload, m_sessionId);
        projectRun();
        emit changed();
        return name == "editAndResendChatMessage" ? QVariant(id) : QVariant(true);
    }
    if (!ipc::desktopActions().contains(name)) {
        emit operationFailed(QStringLiteral("unsupported-desktop-action"));
        return false;
    }
    if (name == "manageMemory" &&
        QJsonDocument(QJsonArray::fromVariantList(args)).toJson(QJsonDocument::Compact).size() >
            ipc::max_frame_bytes - 4096) {
        emit memoryManagementCompleted({{"status", "LimitExceeded"},
                                        {"requestTag", args.first().toMap().value("requestTag")}});
        return false;
    }
    const auto id = send(DaemonClient::Command::desktop_action,
                         {{"action", name},
                          {"arguments", QJsonArray::fromVariantList(args)},
                          {"session_id", m_sessionId}},
                         name);
    if (name == "requestPermanentDeleteConversation")
        m_requests[id].conversationId = text;
    if (name == "manageMemory") {
        if (id.isEmpty()) {
            emit memoryManagementCompleted(
                {{"status", "Unavailable"},
                 {"requestTag", args.first().toMap().value("requestTag")}});
            return false;
        }
        m_requests[id].memoryTag = args.first().toMap().value("requestTag").toString();
    }
    return name == "duplicateConversation" ? QVariant(id) : QVariant(true);
}
void DesktopRuntimeClient::clearSession() {
    m_sessionId.clear();
    m_preferredSession.clear();
    m_attaching = false;
    m_messages.clear();
    m_historyLoaded = false;
    m_runId.clear();
    m_state.clear();
    m_output.clear();
    m_approval = {};
    m_activeAssistantId = 0;
    const auto& fields = projectionFields();
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        if (it.value().toObject().value("scope") == "session")
            m_values.remove(it.key());
    }
}
void DesktopRuntimeClient::attach(const QString& id) {
    if (id.isEmpty() || m_attaching) {
        return;
    }
    if (id != m_sessionId) {
        m_submissionError.clear();
        m_submissionErrorSummary.clear();
    }
    m_attaching = true;
    send(DaemonClient::Command::session_attach, {{"session_id", id}}, id);
}
void DesktopRuntimeClient::requestMessages() {
    if (!m_sessionId.isEmpty()) {
        m_historyLoaded = false;
        send(DaemonClient::Command::session_messages, {{"session_id", m_sessionId}}, m_sessionId);
    }
}
void DesktopRuntimeClient::onResponse(const QString& id, const QString& name,
                                      const QJsonObject& payload) {
    if (!m_requests.contains(id)) {
        return;
    }
    const auto request = m_requests.take(id);
    if (name == "voice.state") {
        m_voiceQueryPending = false;
        m_voiceState = payload.toVariantMap();
        const bool busy = m_voiceDevices.isCapturing() || m_microphonePermissionPending ||
                          m_voiceStartPending || !m_outboundVoicePcm.isEmpty() ||
                          payload.value("state").toString() == "Transcribing";
        m_voiceTimer.setInterval(busy ? 100 : 2000);
    } else if (name == "voice.action" && request.target != "cancel") {
        if (request.target != QString::number(m_voiceGeneration)) {
            return;
        }
        m_voiceStartPending = false;
        m_voiceDevices.setVadEnabled(m_voiceState.value("vad_enabled", true).toBool());
        if (!payload.value("accepted").toBool() ||
            !m_voiceDevices.selectInput(m_voiceState.value("input_device_id").toString()) ||
            !m_voiceDevices.startCapture()) {
            voiceAction("cancel");
            if (m_localVoiceFailure.isEmpty()) {
                m_localVoiceFailure = "CaptureFailure";
            }
            emit operationFailed("voice-capture-unavailable");
        } else {
            m_captureLimit.start();
        }
    } else if (name == "voice.audio") {
        if (request.target != QString::number(m_voiceGeneration)) {
            return;
        }
        if (!payload.value("accepted").toBool()) {
            voiceAction("cancel");
            m_localVoiceFailure = "TranscriptionFailure";
        } else if (m_voiceOffset < m_outboundVoicePcm.size()) {
            sendVoiceChunk();
        } else {
            m_outboundVoicePcm.clear();
        }
    } else if (name == "desktop.settings_service") {
        const auto result = payload.value("result").toObject();
        if (id == m_extensionActionPending) {
            m_extensionActionPending.clear();
            m_extensionActionState["phase"] = result.value("accepted").toBool() ? "accepted" : "failed";
            refreshExtensionsHub();
            return;
        }
        const auto action = request.target.left(request.target.indexOf('['));
        if (ipc::desktopSettingsActions().value(action).toObject().value("query").toBool()) {
            m_settingsStates[request.target] = result.toVariantMap();
        } else {
            if (result.contains("accepted") && !result.value("accepted").toBool()) {
                emit operationFailed(result.value("code").toString("daemon-settings-rejected"));
            }
            m_settingsQueries.clear();
            m_settingsStates.clear();
            refresh();
        }
    } else if (name == "desktop.projection") {
        const int page = payload.value("page").toInt(-1), pages = payload.value("pages").toInt();
        if (page < 0 || pages < 1 || pages > 128 || page >= pages) {
            m_projectionRemaining = 0;
            emit operationFailed("malformed-projection");
            return;
        }
        if (page == 0) {
            m_projection.clear();
        }
        const auto properties = payload.value("properties").toObject();
        if (!validProperties(properties)) {
            m_projectionRemaining = 0;
            emit operationFailed("malformed-projection");
            return;
        }
        for (auto it = properties.begin(); it != properties.end(); ++it) {
            if (!projectionFields().contains(it.key())) {
                emit operationFailed("unknown-projection-field");
                return;
            }
            m_projection[it.key()] = it.value().toVariant();
        }
        if (page == 0) {
            m_projectionRemaining = pages;
            for (int next = 1; next < pages; ++next) {
                send(DaemonClient::Command::desktop_projection, {{"page", next}});
            }
        }
        if (--m_projectionRemaining != 0) {
            return;
        }
        const auto& fields = projectionFields();
        bool complete = true;
        for (auto field = fields.begin(); field != fields.end(); ++field) {
            const auto metadata = field.value().toObject();
            if (metadata.value("eager").toBool() &&
                metadata.value("scope").toString() == "global" &&
                !m_projection.contains(field.key())) {
                complete = false;
                break;
            }
        }
        if (!complete) {
            emit operationFailed("incomplete-projection");
            return;
        }
        for (auto it = m_projection.begin(); it != m_projection.end(); ++it) {
            m_values[it.key()] = it.value();
        }
        m_loaded = true;
    } else if (name == "desktop.settings") {
        m_settings = payload.value("values").toObject();
        m_settingsLoaded = true;
    } else if (name == "desktop.setting") {
        if (!payload.value("accepted").toBool()) {
            emit operationFailed("daemon-setting-rejected");
        }
        send(DaemonClient::Command::desktop_settings);
    } else if (name == "session.list") {
        m_sessions = payload.value("sessions").toArray();
        m_sessionsLoaded = true;
        QStringList ids, titles, summaries;
        for (const auto& item : m_sessions) {
            const auto session = item.toObject();
            ids.append(session.value("session_id").toString());
            titles.append(session.value("title").toString());
            summaries.append(session.value("summary").toString());
            if (session.value("session_id").toString() == m_sessionId && !m_submissionPending) {
                applySnapshot(session);
            }
        }
        m_values["conversationIds"] = ids;
        m_values["conversationTitles"] = titles;
        m_values["conversationSummaries"] = summaries;
        QStringList pinned, archived;
        for (const auto& item : m_sessions) {
            const auto session = item.toObject();
            pinned.append(session.value("pinned").toBool() ? "Pinned" : "Unpinned");
            archived.append(session.value("archived").toBool() ? "Archived" : "Active");
        }
        m_values["conversationPinnedSummaries"] = pinned;
        m_values["conversationArchivedSummaries"] = archived;
        m_values["conversationStoreConversationCount"] = ids.size();
        if (!m_sessionId.isEmpty() && !ids.contains(m_sessionId) &&
            !active(m_state) && !m_submissionPending)
            clearSession();
        if (m_sessionId.isEmpty() && !m_attaching) {
            if (ids.contains(m_preferredSession)) {
                attach(m_preferredSession);
            } else if (!ids.isEmpty()) {
                attach(ids.first());
            } else {
                send(DaemonClient::Command::session_create, {{"title", "New Chat"}});
            }
        }
    } else if (name == "session.create") {
        attach(payload.value("session_id").toString());
        send(DaemonClient::Command::session_list);
    } else if (name == "session.attach") {
        m_attaching = false;
        if (payload.value("session_id").toString() != request.target) {
            return;
        }
        if (m_sessionId != request.target) {
            const auto& fields = projectionFields();
            for (auto it = fields.begin(); it != fields.end(); ++it) {
                if (it.value().toObject().value("scope") == "session") {
                    m_values.remove(it.key());
                }
            }
            m_messages.clear();
            m_historyLoaded = false;
        }
        m_sessionId = request.target;
        m_preferredSession = m_sessionId;
        applySnapshot(payload);
        requestMessages();
        emit sessionChanged(m_sessionId);
    } else if (name == "session.messages") {
        if (request.target != m_sessionId || request.runId != m_runId) {
            return;
        }
        QList<core::ChatMessage> messages;
        for (const auto& item : payload.value("messages").toArray()) {
            const auto row = item.toObject();
            core::ChatMessage message;
            message.id = row.value("id").toInt();
            const auto role = row.value("role").toString();
            message.role = role == "user"        ? core::ChatRole::User
                           : role == "assistant" ? core::ChatRole::Assistant
                                                 : core::ChatRole::System;
            message.content = row.value("content").toString();
            message.status = messageStatus(row.value("status").toString());
            message.timestamp =
                QDateTime::fromString(row.value("timestamp").toString(), Qt::ISODateWithMs);
            message.providerUsed = row.value("provider_id").toString();
            message.modelUsed = row.value("model_id").toString();
            message.partial = row.value("partial").toBool();
            message.replyToMessageId = row.value("reply_to").toInt();
            message.replacesMessageId = row.value("replaces").toInt();
            messages.append(message);
        }
        m_messages = std::move(messages);
        m_historyLoaded = true;
        m_activeAssistantId = m_kind == "chat" && !m_messages.isEmpty() &&
                                      m_messages.last().role == core::ChatRole::Assistant
                                  ? m_messages.last().id
                                  : 0;
    } else if (name == "chat.send" || name == "agent.start" || name == "chat.retry" ||
               name == "chat.regenerate" || name == "chat.edit") {
        m_submissionPending = false;
        if (payload.value("session_id").toString() != m_sessionId) {
            return;
        }
        m_runId = payload.value("run_id").toString();
        m_output.clear();
        m_approval = {};
        m_activeAssistantId = 0;
        m_state = "running";
        requestMessages();
    } else if (name == "approval.respond") {
        m_approvalResponsePending = false;
        attach(m_sessionId);
        send(DaemonClient::Command::session_list);
    } else if (name == "desktop.action" || name == "model.select") {
        if (request.target == "manageMemory") {
            if (m_projectionRemaining == 0) {
                m_projectionRemaining = -1;
                send(DaemonClient::Command::desktop_projection, {{"page", 0}});
            }
            emit memoryManagementCompleted(
                payload.value("result").toObject().value("value").toObject().toVariantMap());
            return; // Memory results never reattach/redirect the accepted conversation.
        }
        if (request.target == "requestPermanentDeleteConversation")
            emit conversationDeleteCompleted(request.conversationId, payload.value("accepted").toBool(),
                payload.value("accepted").toBool() ? QString{} : tr("The conversation store rejected deletion."));
        if (!payload.value("accepted").toBool(true)) {
            emit operationFailed("daemon-action-rejected");
        }
        if (request.target == "requestPermanentDeleteConversation" &&
            payload.value("accepted").toBool() && request.conversationId == m_sessionId)
            clearSession();
        if (request.target == "duplicateConversation" && payload.value("accepted").toBool()) {
            attach(payload.value("result").toObject().value("value").toString());
        }
        refresh();
    }
    if (ready() && !m_sessionId.isEmpty() && m_startupProjectionTimer.isValid()) {
        qDebug() << "Desktop handshake to authoritative projection/session (ms):"
                 << static_cast<double>(m_startupProjectionTimer.nsecsElapsed()) / 1000000.0;
        m_startupProjectionTimer.invalidate();
    }
    projectRun();
    emit changed();
}
void DesktopRuntimeClient::applySnapshot(const QJsonObject& snapshot) {
    const auto generation = snapshot.value("server_generation").toString();
    const auto sequence = static_cast<quint64>(snapshot.value("event_sequence").toDouble());
    if ((!generation.isEmpty() && generation != m_generation) ||
        (sequence && sequence < m_sequence)) {
        return;
    }
    const auto properties = snapshot.value("properties").toObject();
    if (!validProperties(properties)) {
        emit operationFailed("malformed-session-projection");
        return;
    }
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (projectionFields().contains(it.key())) {
            m_values[it.key()] = it.value().toVariant();
        }
    }
    m_values["activeConversationTitle"] = snapshot.value("title").toString();
    m_values["conversationListCurrentTitle"] = snapshot.value("title").toString();
    m_values["activeConversationSummary"] = snapshot.value("summary").toString();
    m_values["activeConversationPinned"] = snapshot.value("pinned").toBool();
    m_values["activeConversationArchived"] = snapshot.value("archived").toBool();
    if (m_runId != snapshot.value("run_id").toString()) {
        m_historyLoaded = false;
    }
    m_runId = snapshot.value("run_id").toString();
    m_kind = snapshot.value("run_type").toString();
    m_state = snapshot.value("state").toString();
    m_output = snapshot.value("output").toString();
    m_approval = snapshot.value("approval").toObject();
    if (m_state != "approval" || snapshot.value("approval_id").toString().isEmpty()) {
        m_approval = {};
    } else {
        m_approval.insert("approval_id", snapshot.value("approval_id"));
    }
    if (sequence) {
        m_sequence = sequence;
    }
    m_values["activeChatProviderId"] = snapshot.value("provider_id").toString();
    m_values["activeChatModelId"] = snapshot.value("model_id").toString();
    m_values["conversationRuntimeActiveModel"] = snapshot.value("model_id").toString();
}
void DesktopRuntimeClient::onEvent(const QString& name, const QJsonObject& payload) {
    if (payload.value("session_id").toString() != m_sessionId ||
        payload.value("run_id").toString() != m_runId) {
        return;
    }
    // Closed runs cannot advance the cursor used by the next run or revive its state.
    if (!active(m_state)) {
        return;
    }
    const auto generation = payload.value("server_generation").toString();
    if (!generation.isEmpty() && generation != m_generation) {
        return;
    }
    const auto sequence = static_cast<quint64>(payload.value("event_sequence").toDouble());
    if (sequence && sequence <= m_sequence) {
        return;
    }
    if (sequence) {
        m_sequence = sequence;
    }
    if (name == "agent.activity") {
        m_values["latestAgentActivitySummary"] = payload.value("activity").toString();
    } else if (name == "output.delta") {
        if (!active(m_state)) {
            return;
        }
        m_output += payload.value("text").toString();
    } else if (name == "approval.requested") {
        m_approval = payload;
        m_state = "approval";
    } else if (name == "tool.requested" || name == "tool.result") {
        m_values["latestToolExecutionSummary"] =
            payload.value("tool").toString() + ": " + payload.value("detail").toString();
        m_values["latestAgentActivitySummary"] = m_values["latestToolExecutionSummary"];
    } else if (name == "run.started") {
        applySnapshot(payload);
    } else if (name == "run.completed" || name == "run.failed" || name == "run.cancelled") {
        if (!active(m_state)) {
            return;
        }
        m_state = payload.value("state").toString();
        m_output = payload.value("text").toString();
        if (name == "run.failed") {
            m_values["chatErrorCategory"] = payload.value("detail").toString();
            const auto detail = payload.value("detail").toString();
            m_values["chatSendLifecycleSummary"] =
                detail == "ProviderUnavailable" || detail == "ConnectionFailed"
                    ? tr("The selected provider is unavailable. Check that its server is running "
                         "and its endpoint is correct in Settings.")
                    : detail;
        }
        m_approval = {};
        m_approvalResponsePending = false;
        requestMessages();
        send(DaemonClient::Command::session_list);
    }
    projectRun();
    emit changed();
}
QVariantMap DesktopRuntimeClient::quickPanelSnapshot() const {
    QJsonObject approval = m_approval;
    QString approvalSession = m_sessionId, approvalRun = m_runId;
    int count = m_approval.isEmpty() ? 0 : 1;
    for (const auto& row : m_sessions) {
        const auto session = row.toObject();
        const auto pending = session.value("approval").toObject();
        if (session.value("session_id").toString() == m_sessionId) {
            continue;
        }
        if (session.value("state").toString() != "approval" || pending.isEmpty()) {
            continue;
        }
        ++count;
        if (approval.isEmpty()) {
            approval = pending;
            approval.insert("approval_id", session.value("approval_id"));
            approvalSession = session.value("session_id").toString();
            approvalRun = session.value("run_id").toString();
        }
    }
    return {{"sessionId", m_sessionId},
            {"runId", m_runId},
            {"kind", m_kind},
            {"state", m_submissionPending ? QStringLiteral("starting") : m_state},
            {"preview", m_kind == "agent" && (active(m_state) || m_submissionPending)
                            ? QString{}
                            : m_output.right(2000)},
            {"approval", approval.toVariantMap()},
            {"approvalSession", approvalSession},
            {"approvalRun", approvalRun},
            {"approvalCount", m_transport.daemonReachable() ? count : 0},
            {"approvalPending", m_approvalResponsePending},
            {"busy", active(m_state) || m_submissionPending},
            {"activity", m_values.value("latestToolExecutionSummary")}};
}
bool DesktopRuntimeClient::respondToQuickApproval(bool allow) {
    const auto snapshot = quickPanelSnapshot();
    const auto approval = snapshot.value("approval").toMap();
    if (!ready() || m_approvalResponsePending ||
        approval.value("approval_id").toString().isEmpty()) {
        return false;
    }
    m_approvalResponsePending = true;
    send(DaemonClient::Command::approval_respond,
         {{"run_id", snapshot.value("approvalRun").toString()},
          {"approval_id", approval.value("approval_id").toString()},
          {"allow", allow}},
         snapshot.value("approvalSession").toString());
    emit changed();
    return true;
}
void DesktopRuntimeClient::projectRun() {
    const bool connected = m_transport.daemonReachable();
    const bool busy = connected && (active(m_state) || m_submissionPending);
    m_values["agentLoopActive"] = busy && m_kind == "agent";
    m_values["agentAwaitingApproval"] = connected && m_state == "approval" && !m_approval.isEmpty();
    m_values["agentStatus"] = !connected ? m_transport.statusSummary()
                              : !ready() ? QStringLiteral("Loading authoritative state…")
                                         : (busy && m_kind == "agent" ? "Busy" : "Ready");
    m_values["lastAgentResponse"] =
        m_kind == "agent" ? m_output : m_values.value("lastAgentResponse");
    m_values["chatGenerationActive"] = busy && m_kind == "chat";
    m_values["localInferenceStreaming"] = busy && m_kind == "chat";
    m_values["conversationRuntimeStreaming"] = busy && m_kind == "chat";
    m_values["conversationRuntimeRequestId"] = m_runId;
    m_values["activeConversationId"] = m_sessionId;
    m_values["conversationSessionId"] = m_sessionId;
    m_values["conversationState"] = !connected ? "Disconnected" : m_state;
    m_values["chatSendLifecycleState"] = m_submissionPending ? "queued"
                                         : busy && m_kind == "chat"
                                             ? (m_output.isEmpty() ? "sending" : "streaming")
                                             : m_state;
    if (!m_submissionError.isEmpty()) {
        // Session polling restores server metadata, but cannot erase a rejected
        // submission: no authoritative run was created for that request.
        m_values["chatSendLifecycleState"] = QStringLiteral("failed");
        m_values["chatErrorCategory"] = m_submissionError;
        m_values["chatSendLifecycleSummary"] = m_submissionErrorSummary;
    }
    // Match ApplicationController semantics: system-only transcripts (e.g. the
    // startup "Sentinel Core online." notice) are not chat history, so the
    // greeting screen keeps showing until a user or assistant message exists.
    m_values["conversationHistoryMessageCount"] = static_cast<int>(std::count_if(
        m_messages.cbegin(), m_messages.cend(), [](const core::ChatMessage& message) {
            return message.role != core::ChatRole::System;
        }));
    m_values["latestApprovalSummary"] = m_approval.value("detail").toString();
    if (m_kind == "chat" && !m_submissionPending && !m_messages.isEmpty() &&
        m_messages.last().role == core::ChatRole::Assistant && !m_runId.isEmpty() &&
        m_messages.last().id == m_activeAssistantId) {
        auto& message = m_messages.last();
        if (busy) {
            message.content = m_output;
        }
        message.partial = busy;
        message.status = messageStatus(m_state);
    }
}
bool DesktopRuntimeClient::respondToApproval(bool allow) {
    if (!ready() || m_approvalResponsePending || m_state != "approval" ||
        m_approval.value("approval_id").toString().isEmpty()) {
        return false;
    }
    m_approvalResponsePending = true;
    send(DaemonClient::Command::approval_respond,
         {{"run_id", m_runId}, {"approval_id", m_approval.value("approval_id")}, {"allow", allow}},
         m_sessionId);
    return true;
}
bool DesktopRuntimeClient::cancelRun() {
    if (!ready() || !active(m_state) || m_runId.isEmpty()) {
        return false;
    }
    send(DaemonClient::Command::run_cancel, {{"run_id", m_runId}}, m_sessionId);
    return true;
}
} // namespace sentinel::desktop
