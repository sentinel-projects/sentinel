#include "sentinel/desktop/ExtensionsHubPresentation.h"
#include "sentinel/desktop/DesktopBackupHelper.h"
// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/desktop/DesktopShellViewModel.h"
#include "sentinel/core/app/ControlledTaskService.h"
#include "sentinel/core/chat/ChatAttachmentLoader.h"

#include "sentinel/core/app/AppMetadata.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/ApplicationController.h"
#include "sentinel/core/app/ModeManager.h"
#include "sentinel/core/app/OnboardingService.h"
#include "sentinel/core/app/ProductMessages.h"
#include "sentinel/core/app/SettingsService.h"
#include "sentinel/core/model/ModelRegistry.h"

#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QNetworkAccessManager>
#include <QNetworkInformation>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QSslSocket>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <functional>

namespace sentinel::desktop {

namespace {

QStringList numericParts(const QString& version) {
    QStringList parts;
    QString current;
    for (const QChar c : version) {
        if (c.isDigit()) {
            current += c;
        } else if (!current.isEmpty()) {
            parts.append(current);
            current.clear();
        }
    }
    if (!current.isEmpty())
        parts.append(current);
    return parts;
}

bool isNewerVersion(const QString& current, const QString& latest) {
    auto stripV = [](QString v) {
        if (v.startsWith(u'v') || v.startsWith(u'V'))
            return v.mid(1);
        return v;
    };
    const QStringList curParts = numericParts(stripV(current));
    const QStringList latParts = numericParts(stripV(latest));
    const int maxLen = qMax(curParts.size(), latParts.size());
    for (int i = 0; i < maxLen; ++i) {
        const int curNum = i < curParts.size() ? curParts[i].toInt() : 0;
        const int latNum = i < latParts.size() ? latParts[i].toInt() : 0;
        if (latNum > curNum)
            return true;
        if (latNum < curNum)
            return false;
    }
    return false;
}

QString platformAssetSuffix() {
#ifdef Q_OS_MACOS
    return QStringLiteral(".dmg");
#elif defined(Q_OS_LINUX)
    return QStringLiteral(".AppImage");
#elif defined(Q_OS_WIN)
    return QStringLiteral(".exe");
#else
    return QString();
#endif
}

QString appDataPath() {
    const auto path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return path.isEmpty() ? QDir::currentPath() : path;
}

QString localRagPath() {
    return QDir(appDataPath()).filePath(QStringLiteral("local_rag.sqlite3"));
}

QString attachmentId(const QString& workspaceId, const QString& name, qint64 size) {
    const auto seed =
        workspaceId + QStringLiteral("|") + name + QStringLiteral("|") + QString::number(size);
    return QString::fromLatin1(
        QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
}

QString fileTypeForPath(const QString& pathOrName) {
    const auto suffix = QFileInfo(pathOrName).suffix().toLower();
    if (suffix == QStringLiteral("pdf")) {
        return QStringLiteral("PDF");
    }
    if (QStringList({QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                     QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("bmp")})
            .contains(suffix)) {
        return QStringLiteral("Image");
    }
    if (suffix == QStringLiteral("txt")) {
        return QStringLiteral("TXT");
    }
    if (suffix == QStringLiteral("md") || suffix == QStringLiteral("markdown")) {
        return QStringLiteral("Markdown");
    }
    if (suffix == QStringLiteral("docx")) {
        return QStringLiteral("DOCX");
    }
    if (suffix == QStringLiteral("csv")) {
        return QStringLiteral("CSV");
    }
    if (suffix == QStringLiteral("json")) {
        return QStringLiteral("JSON");
    }
    if (QStringList({QStringLiteral("cpp"), QStringLiteral("h"), QStringLiteral("hpp"),
                     QStringLiteral("qml"), QStringLiteral("js"), QStringLiteral("ts"),
                     QStringLiteral("py"), QStringLiteral("java"), QStringLiteral("cs"),
                     QStringLiteral("go"), QStringLiteral("rs"), QStringLiteral("swift")})
            .contains(suffix)) {
        return QStringLiteral("Source Code");
    }
    return QStringLiteral("Unsupported");
}

bool supportedAttachmentType(const QString& type) {
    return type != QStringLiteral("Unsupported");
}

QJsonArray notificationsFromJson(const QString& json) {
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    return document.object().value(QStringLiteral("notifications")).toArray();
}

QString notificationsToJson(const QJsonArray& notifications, const QString& originalJson) {
    auto root = QJsonDocument::fromJson(originalJson.toUtf8()).object();
    root.insert(QStringLiteral("notifications"), notifications);
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

QString notificationSummary(const QJsonObject& item) {
    const auto state = item.value(QStringLiteral("archived")).toBool() ? QStringLiteral("Archived")
                       : item.value(QStringLiteral("read")).toBool()   ? QStringLiteral("Read")
                                                                       : QStringLiteral("Unread");
    const auto pin = item.value(QStringLiteral("pinned")).toBool() ? QStringLiteral("Pinned")
                                                                   : QStringLiteral("Unpinned");
    return QStringLiteral("%1 - %2 - %3 - %4")
        .arg(item.value(QStringLiteral("category")).toString(),
             item.value(QStringLiteral("title")).toString(), state, pin);
}

QJsonObject notificationJsonObject(const QJsonObject& item) {
    QJsonObject obj;
    obj.insert(QStringLiteral("id"), item.value(QStringLiteral("id")).toString());
    obj.insert(QStringLiteral("category"), item.value(QStringLiteral("category")).toString());
    obj.insert(QStringLiteral("title"), item.value(QStringLiteral("title")).toString());
    obj.insert(QStringLiteral("body"), item.value(QStringLiteral("body")).toString());
    obj.insert(QStringLiteral("priority"),
               item.value(QStringLiteral("priority")).toString(QStringLiteral("Normal")));
    obj.insert(
        QStringLiteral("timestamp"),
        item.value(QStringLiteral("timestamp")).toDouble(QDateTime::currentMSecsSinceEpoch()));
    obj.insert(QStringLiteral("pinned"), item.value(QStringLiteral("pinned")).toBool());
    obj.insert(QStringLiteral("archived"), item.value(QStringLiteral("archived")).toBool());
    obj.insert(QStringLiteral("read"), item.value(QStringLiteral("read")).toBool());
    obj.insert(QStringLiteral("snoozed"), item.value(QStringLiteral("snoozed")).toBool());
    obj.insert(QStringLiteral("snoozeUntil"), item.value(QStringLiteral("snoozeUntil")));
    return obj;
}

bool updateNotification(const QString& json, const QString& notificationId,
                        const std::function<void(QJsonObject&)>& update, QString* updatedJson) {
    auto notifications = notificationsFromJson(json);
    bool changed = false;
    for (qsizetype i = 0; i < notifications.size(); ++i) {
        auto item = notifications.at(i).toObject();
        if (item.value(QStringLiteral("id")).toString() != notificationId) {
            continue;
        }
        update(item);
        notifications.replace(i, item);
        changed = true;
        break;
    }
    if (!changed) {
        return false;
    }
    *updatedJson = notificationsToJson(notifications, json);
    return true;
}

} // namespace

namespace {
constexpr int kDaemonUnavailableGraceMs = 5000;
} // namespace

DesktopShellViewModel::DesktopShellViewModel(core::ApplicationController& controller,
                                             core::ModeManager& modeManager,
                                             core::AppSettings& settings,
                                             core::WinTaskbarIntegration* taskbar, QObject* parent)
    : DesktopShellViewModel(DesktopControllerBridge::Source{&controller, nullptr}, modeManager,
                            settings, taskbar, parent) {}

DesktopShellViewModel::DesktopShellViewModel(DesktopRuntimeClient& client,
                                             core::ModeManager& modeManager,
                                             core::AppSettings& settings,
                                             core::WinTaskbarIntegration* taskbar, QObject* parent)
    : DesktopShellViewModel(DesktopControllerBridge::Source{nullptr, &client}, modeManager,
                            settings, taskbar, parent) {
    connect(&client.transport(), &DaemonClient::connectionStateChanged, this,
            [this] { refreshDaemonErrorProjection(); });
    connect(&client, &DesktopRuntimeClient::changed, this,
            &DesktopShellViewModel::runtimeProjectionChanged);
    connect(&client, &DesktopRuntimeClient::memoryManagementCompleted, this,
            &DesktopShellViewModel::memoryManagementCompleted);
    connect(&client, &DesktopRuntimeClient::conversationDeleteCompleted, this,
            &DesktopShellViewModel::conversationDeleteCompleted);
    connect(&client, &DesktopRuntimeClient::changed, this, [this, &client] {
        if (attachmentSendPending_) {
            const auto state = client.value("chatSendLifecycleState").toString();
            if (state == "sending" || state == "streaming" || state == "running" || state == "completed") {
                attachmentSendPending_ = false; clearAttachments();
            } else if (state == "failed") attachmentSendPending_ = false;
        }
        const auto voice = client.voiceState();
        const bool recording = systemDictation_.active() || voice.value("state").toString() == "Listening";
        if (voiceRecordingActive_ != recording) { voiceRecordingActive_ = recording; emit voiceRecordingActiveChanged(); }
        const auto transcript = voice.value("transcript").toString();
        if (!transcript.isEmpty() && transcript != lastVoiceTranscript_) {
            lastVoiceTranscript_ = transcript; voiceInputStatus_.clear(); emit voiceInputChanged(); deliverVoiceTranscript(transcript);
        }
        const auto failure = voice.value("failure").toString();
        if (!failure.isEmpty() && failure != "None" && voice.value("state").toString() == "Failed") {
            voiceInputStatus_ = failure; emit voiceInputChanged();
        }
    });
    connect(&client, &DesktopRuntimeClient::operationFailed, this, [this](const QString& code) {
        if (attachmentSendPending_ || !attachments_.isEmpty()) { attachmentSendPending_ = false; attachmentError_ = code; emit attachmentChanged(); }
        addNotification(tr("System"), tr("Daemon request failed"), code);
    });
}

DesktopShellViewModel::DesktopShellViewModel(DesktopControllerBridge::Source source,
                                             core::ModeManager& modeManager,
                                             core::AppSettings& settings,
                                             core::WinTaskbarIntegration* taskbar, QObject* parent)
    : QObject(parent), controller_(source, this), modeManager_(modeManager), settings_(settings),
      localRagStore_(source.local ? std::make_unique<core::LocalRagStore>(localRagPath())
                                  : nullptr),
      chatMessages_(this), taskbar_(taskbar) {
    backupManager_ = new DesktopBackupHelper(controller_.remote(), this);
    daemonGraceTimer_ = new QTimer(this);
    daemonGraceTimer_->setSingleShot(true);
    daemonGraceTimer_->setInterval(kDaemonUnavailableGraceMs);
    connect(daemonGraceTimer_, &QTimer::timeout, this, [this] {
        auto* runtime = controller_.remote();
        if (!runtime)
            return;
        const auto state = runtime->transport().connectionState();
        if (state != DaemonClient::ConnectionState::Connected &&
            state != DaemonClient::ConnectionState::Reconnecting)
            showDaemonUnavailableBanner();
    });
    connect(&systemDictation_, &SystemDictationService::activeChanged, this, [this] {
        voiceRecordingActive_ = systemDictation_.active(); emit voiceRecordingActiveChanged();
    });
    connect(&systemDictation_, &SystemDictationService::completed, this, [this](const QString& text) {
        voiceInputStatus_.clear(); emit voiceInputChanged(); deliverVoiceTranscript(text);
    });
    connect(&systemDictation_, &SystemDictationService::failed, this, [this](const QString& message) {
        voiceInputStatus_ = message; emit voiceInputChanged();
    });
    if (!controller_.isRemote())
        controller_.attachControlledTaskSettings(settings_);
    if (auto* session = controller_.audioSession()) {
        connect(session, &core::VoiceSessionService::stateChanged, this,
                [this](core::VoiceInteractionState state) {
                    const bool listening = state == core::VoiceInteractionState::Listening;
                    if (voiceRecordingActive_ == listening)
                        return;
                    voiceRecordingActive_ = listening;
                    emit voiceRecordingActiveChanged();
                });
        connect(session, &core::VoiceSessionService::finalTranscriptChanged, this,
                &DesktopShellViewModel::deliverVoiceTranscript);
        connect(session, &core::VoiceSessionService::failed, this,
                [this](core::AudioFailure failure, const QString& detail) {
                    voiceInputStatus_ = QStringLiteral("%1: %2").arg(core::audioFailureName(failure), detail);
                    emit voiceInputChanged();
                });
    }
    if (!controller_.isRemote())
        controller_.setToolPermissionPolicyState(settings_.defaultPermissionPolicyState());
    if (auto* controlledTasks = controller_.controlledTasks())
        connect(controlledTasks, &core::ControlledTaskService::tasksChanged, this,
                &DesktopShellViewModel::controlledAgentTasksChanged);
    // ── Network connectivity monitoring ────────────────────────────────────
    if (!QNetworkInformation::instance()) {
        QNetworkInformation::loadDefaultBackend();
    }
    if (auto* ni = QNetworkInformation::instance()) {
        QObject::connect(ni, &QNetworkInformation::reachabilityChanged, this,
                         [this]() { emit onlineChanged(); });
    }

    lastAgentStatus_ = controller_.agentStatus();
    const QString savedMode = settings_.selectedSystemMode();
    if (!savedMode.isEmpty()) {
        modeManager_.setModeByName(savedMode);
    }
    if (!controller_.isRemote())
        controller_.setRoutingModeByName(settings_.routingModeName());
    chatMessages_.setMessages(controller_.chatHistory());
    connect(&controller_, &DesktopControllerBridge::chatMessagesChanged, this, [this]() {
        chatMessages_.setMessages(controller_.chatHistory());
        emit chatMessagesChanged();
        emit providerStatusChanged();
    });
    connect(&controller_, &DesktopControllerBridge::memoryEntriesChanged, this,
            &DesktopShellViewModel::memoryEntriesChanged);
    connect(&controller_, &DesktopControllerBridge::maintenanceStatusChanged, this,
            &DesktopShellViewModel::maintenanceStatusChanged);
    connect(&controller_, &DesktopControllerBridge::agentStatusChanged, this, [this]() {
        emit agentStatusChanged();
        emit agentLoopChanged();
        emit providerStatusChanged();
        const QString currentStatus = controller_.agentStatus();
        if (currentStatus == QLatin1String("Ready") && lastAgentStatus_ == QLatin1String("Busy")) {
            const QString response = controller_.lastAgentResponse();
            if (!response.isEmpty()) {
                addNotification(
                    tr("Agent"), tr("Sentinel Assistant"),
                    tr("New response received: \"%1\"")
                        .arg(response.left(120) + (response.length() > 120 ? QStringLiteral("...")
                                                                           : QStringLiteral(""))));
            }
        }
        lastAgentStatus_ = currentStatus;
    });
    connect(&controller_, &DesktopControllerBridge::agentResponseChanged, this,
            &DesktopShellViewModel::agentResponseChanged);
    connect(&controller_, &DesktopControllerBridge::toolPlanChanged, this,
            &DesktopShellViewModel::toolPlanChanged);
    connect(&controller_, &DesktopControllerBridge::approvalChanged, this,
            &DesktopShellViewModel::approvalChanged);
    connect(&controller_, &DesktopControllerBridge::sandboxChanged, this,
            &DesktopShellViewModel::sandboxChanged);
    connect(&controller_, &DesktopControllerBridge::toolExecutionChanged, this,
            &DesktopShellViewModel::toolExecutionChanged);
    connect(&controller_, &DesktopControllerBridge::agentPipelineChanged, this,
            &DesktopShellViewModel::agentPipelineChanged);
    connect(&controller_, &DesktopControllerBridge::runtimeContextChanged, this,
            &DesktopShellViewModel::runtimeContextChanged);
    connect(&controller_, &DesktopControllerBridge::conversationSessionChanged, this,
            &DesktopShellViewModel::conversationSessionChanged);
    connect(&controller_, &DesktopControllerBridge::conversationStateChanged, this, [this]() {
        emit conversationStateChanged();
        emit agentLoopChanged();
    });
    connect(&controller_, &DesktopControllerBridge::agentLoopStateChanged, this,
            &DesktopShellViewModel::agentLoopChanged);
    connect(&controller_, &DesktopControllerBridge::conversationRuntimeChanged, this,
            &DesktopShellViewModel::conversationRuntimeChanged);
    connect(&controller_, &DesktopControllerBridge::conversationSearchChanged, this,
            &DesktopShellViewModel::conversationSearchChanged);
    connect(&controller_, &DesktopControllerBridge::conversationExportChanged, this,
            &DesktopShellViewModel::conversationExportChanged);
    connect(&controller_, &DesktopControllerBridge::conversationDuplicateChanged, this,
            &DesktopShellViewModel::conversationDuplicateChanged);
    connect(&controller_, &DesktopControllerBridge::conversationDeleteChanged, this,
            &DesktopShellViewModel::conversationDeleteChanged);
    connect(&controller_, &DesktopControllerBridge::memoryCandidatesChanged, this,
            &DesktopShellViewModel::memoryCandidatesChanged);
    connect(&controller_, &DesktopControllerBridge::memoryRecallChanged, this,
            &DesktopShellViewModel::memoryRecallChanged);
    connect(&controller_, &DesktopControllerBridge::contextAssemblyChanged, this,
            &DesktopShellViewModel::contextAssemblyChanged);
    connect(&controller_, &DesktopControllerBridge::agentActivityChanged, this,
            &DesktopShellViewModel::agentActivityChanged);
    connect(&controller_, &DesktopControllerBridge::modelRoutingChanged, this,
            &DesktopShellViewModel::modelRoutingChanged);
    connect(&controller_, &DesktopControllerBridge::taskPlanChanged, this,
            &DesktopShellViewModel::taskPlanChanged);
    connect(&controller_, &DesktopControllerBridge::orchestrationSnapshotChanged, this,
            &DesktopShellViewModel::orchestrationSnapshotChanged);
    connect(&controller_, &DesktopControllerBridge::runtimeProviderRegistryChanged, this,
            &DesktopShellViewModel::runtimeProviderRegistryChanged);
    connect(&controller_, &DesktopControllerBridge::localModelSelectionChanged, this,
            &DesktopShellViewModel::localModelSelectionChanged);
    connect(&controller_, &DesktopControllerBridge::localModelSelectionChanged, this,
            &DesktopShellViewModel::modelCapabilitySettingsChanged);
    connect(&controller_, &DesktopControllerBridge::modelCapabilitiesChanged, this,
            &DesktopShellViewModel::modelCapabilitySettingsChanged);
    connect(&controller_, &DesktopControllerBridge::ollamaStatusChanged, this,
            &DesktopShellViewModel::ollamaStatusChanged);
    connect(&controller_, &DesktopControllerBridge::ollamaStatusChanged, this,
            &DesktopShellViewModel::providerStatusChanged);
    connect(&controller_, &DesktopControllerBridge::localChatInferenceRoutingChanged, this,
            &DesktopShellViewModel::localChatInferenceRoutingChanged);
    connect(&controller_, &DesktopControllerBridge::localInferenceChanged, this,
            &DesktopShellViewModel::localInferenceChanged);
    connect(&settings_, &core::AppSettings::cloudApiKeysChanged, this, [this]() {
        // Refresh cloud model discovery immediately when credentials change
        // instead of waiting for the next poll tick.
        controller_.refreshModelDiscovery();
        emit cloudApiKeysChanged();
    });
    connect(&settings_, &core::AppSettings::proxySettingsChanged, this,
            &DesktopShellViewModel::proxySettingsChanged);
    connect(&settings_, &core::AppSettings::selectedCloudProviderChanged, this, [this]() {
        // Provider switch changes which cloud endpoint must be discovered.
        controller_.refreshModelDiscovery();
        emit selectedCloudProviderChanged();
    });
    connect(&settings_, &core::AppSettings::webSearchSettingsChanged, this, [this]() {
        controller_.configureWebSearch(settings_.webSearchProvider(), settings_.webSearchApiKey(),
                                       settings_.webSearchMaxResults());
        emit webSearchSettingsChanged();
    });
    connect(&settings_, &core::AppSettings::mcpServersChanged, this, [this]() {
        controller_.configureMcpServers(settings_.mcpServersJson());
        emit mcpServersChanged();
    });
    connect(&settings_, &core::AppSettings::semanticSettingsChanged, this, [this]() {
        controller_.setSemanticProvider(settings_.semanticProvider(),
                                        settings_.semanticEmbeddingModel());
        emit semanticSettingsChanged();
        emit contextAssemblyChanged();
    });
    connect(&controller_, &DesktopControllerBridge::promptContextInjectionChanged, this,
            &DesktopShellViewModel::promptContextInjectionChanged);
    connect(&controller_, &DesktopControllerBridge::voiceConfigurationChanged, this,
            &DesktopShellViewModel::voiceConfigurationChanged);
    connect(&modeManager_, &core::ModeManager::currentModeChanged, this,
            &DesktopShellViewModel::currentModeChanged);
    connect(&settings_, &core::AppSettings::themeNameChanged, this,
            &DesktopShellViewModel::themeNameChanged);
    connect(&settings_, &core::AppSettings::configurationProfileChanged, this,
            &DesktopShellViewModel::configurationProfileChanged);
    connect(&settings_, &core::AppSettings::appLanguageChanged, this,
            &DesktopShellViewModel::appLanguageChanged);
    connect(&settings_, &core::AppSettings::appLanguageChanged, this,
            &DesktopShellViewModel::localChatInferenceRoutingChanged);
    connect(&settings_, &core::AppSettings::companionEnabledChanged, this,
            &DesktopShellViewModel::companionChanged);
    connect(&settings_, &core::AppSettings::developerModeEnabledChanged, this,
            &DesktopShellViewModel::developerModeChanged);
    connect(&settings_, &core::AppSettings::agentAutonomousModeChanged, this,
            &DesktopShellViewModel::agentAutonomousModeChanged);
    connect(&settings_, &core::AppSettings::selectedSystemModeChanged, this,
            &DesktopShellViewModel::currentModeChanged);
    connect(&settings_, &core::AppSettings::updateCheckPolicyChanged, this, [this]() {
        emit nativeExperienceChanged();
        setupBackgroundUpdateCheck();
    });
    connect(&settings_, &core::AppSettings::notificationPolicyChanged, this,
            &DesktopShellViewModel::nativeExperienceChanged);
    connect(&settings_, &core::AppSettings::onboardingCompleteChanged, this,
            &DesktopShellViewModel::nativeExperienceChanged);
    connect(&settings_, &core::AppSettings::onboardingUseCaseChanged, this,
            &DesktopShellViewModel::nativeExperienceChanged);
    connect(&settings_, &core::AppSettings::onboardingUseCaseChanged, this,
            &DesktopShellViewModel::onboardingStateChanged);
    connect(&settings_, &core::AppSettings::onboardingCompleteChanged, this,
            &DesktopShellViewModel::onboardingStateChanged);
    connect(&settings_, &core::AppSettings::recoveryDraftTextChanged, this,
            &DesktopShellViewModel::recoveryDraftTextChanged);
    connect(&settings_, &core::AppSettings::productExperienceChanged, this,
            &DesktopShellViewModel::nativeExperienceChanged);
    connect(&settings_, &core::AppSettings::controlledAgentTasksChanged, this,
            &DesktopShellViewModel::controlledAgentTasksChanged);
    connect(&settings_, &core::AppSettings::selectedSkillProfileChanged, this,
            &DesktopShellViewModel::skillProfileChanged);
    connect(&settings_, &core::AppSettings::selectedSkillProfileChanged, this,
            &DesktopShellViewModel::agentRuntimeChanged);
    connect(&settings_, &core::AppSettings::selectedWorkspaceIdChanged, this,
            &DesktopShellViewModel::workspaceChanged);
    connect(&settings_, &core::AppSettings::workspaceSettingsChanged, this,
            &DesktopShellViewModel::workspaceChanged);
    connect(&settings_, &core::AppSettings::selectedWorkspaceIdChanged, this,
            &DesktopShellViewModel::agentRuntimeChanged);
    connect(&settings_, &core::AppSettings::defaultPermissionPolicyStateChanged, this,
            &DesktopShellViewModel::permissionPolicyChanged);
    connect(&settings_, &core::AppSettings::defaultPermissionPolicyStateChanged, this, [this]() {
        controller_.setToolPermissionPolicyState(settings_.defaultPermissionPolicyState());
    });
    connect(&settings_, &core::AppSettings::defaultPermissionPolicyStateChanged, this,
            &DesktopShellViewModel::agentRuntimeChanged);
    connect(&settings_, &core::AppSettings::contextExplainabilityVisibleChanged, this,
            &DesktopShellViewModel::contextExplainabilityVisibleChanged);
    connect(&settings_, &core::AppSettings::activeConversationIdChanged, this,
            &DesktopShellViewModel::chatMessagesChanged);
    connect(&settings_, &core::AppSettings::routingModeNameChanged, this,
            &DesktopShellViewModel::modelRoutingChanged);
    connect(&settings_, &core::AppSettings::ollamaEndpointChanged, this,
            &DesktopShellViewModel::ollamaStatusChanged);
    connect(&settings_, &core::AppSettings::lmStudioEndpointChanged, this,
            &DesktopShellViewModel::ollamaStatusChanged);
    connect(&settings_, &core::AppSettings::llamaCppEndpointChanged, this,
            &DesktopShellViewModel::ollamaStatusChanged);
    connect(&settings_, &core::AppSettings::cloudApiEndpointChanged, this,
            &DesktopShellViewModel::ollamaStatusChanged);
    connect(&settings_, &core::AppSettings::selectedLocalModelChanged, this, [this]() {
        controller_.setSelectedLocalModel(
            settings_.selectedModelForProvider(settings_.selectedRuntimeProvider()));
    });
    connect(&settings_, &core::AppSettings::selectedModelRoleChanged, this,
            &DesktopShellViewModel::modelRoleChanged);
    connect(&settings_, &core::AppSettings::selectedRuntimeProviderChanged, this, [this]() {
        controller_.setSelectedRuntimeProvider(settings_.selectedRuntimeProvider());
        controller_.setSelectedLocalModel(
            settings_.selectedModelForProvider(settings_.selectedRuntimeProvider()));
    });
    connect(&settings_, &core::AppSettings::localChatInferenceEnabledChanged, this, [this]() {
        controller_.setLocalChatInferenceEnabled(settings_.localChatInferenceEnabled());
    });
    connect(&settings_, &core::AppSettings::localInferenceStreamingEnabledChanged, this, [this]() {
        controller_.setLocalInferenceStreamingEnabled(settings_.localInferenceStreamingEnabled());
    });
    connect(&settings_, &core::AppSettings::localInferenceTimeoutMsChanged, this, [this]() {
        controller_.setLocalInferenceTimeoutMs(settings_.localInferenceTimeoutMs());
    });
    connect(&settings_, &core::AppSettings::localInferenceTemperatureChanged, this, [this]() {
        controller_.setLocalInferenceTemperature(settings_.localInferenceTemperature());
    });
    connect(&settings_, &core::AppSettings::localInferenceTopPChanged, this,
            [this]() { controller_.setLocalInferenceTopP(settings_.localInferenceTopP()); });
    connect(&settings_, &core::AppSettings::localInferenceMaxTokensChanged, this, [this]() {
        controller_.setLocalInferenceMaxTokens(settings_.localInferenceMaxTokens());
    });
    connect(&settings_, &core::AppSettings::promptContextInjectionEnabledChanged, this, [this]() {
        controller_.setPromptContextInjectionEnabled(settings_.promptContextInjectionEnabled());
    });
    connect(&settings_, &core::AppSettings::semanticPromptInclusionEnabledChanged, this, [this]() {
        controller_.setSemanticPromptInclusionEnabled(settings_.semanticPromptInclusionEnabled());
    });
    connect(&settings_, &core::AppSettings::piperBinaryPathChanged, this,
            [this]() { controller_.setPiperBinaryPath(settings_.piperBinaryPath()); });
    connect(&settings_, &core::AppSettings::piperModelPathChanged, this,
            [this]() { controller_.setPiperModelPath(settings_.piperModelPath()); });
    auto refreshSpeechTts = [this] {
        controller_.configureSpeechTts(settings_.selectedTtsEngine(), settings_.kokoroModelPath(),
                                       settings_.kokoroVoice());
        emit voiceConfigurationChanged();
    };
    connect(&settings_, &core::AppSettings::selectedTtsEngineChanged, this, refreshSpeechTts);
    connect(&settings_, &core::AppSettings::kokoroModelPathChanged, this, refreshSpeechTts);
    connect(&settings_, &core::AppSettings::kokoroVoiceChanged, this, refreshSpeechTts);
    connect(&settings_, &core::AppSettings::whisperBinaryPathChanged, this,
            [this]() { controller_.setWhisperBinaryPath(settings_.whisperBinaryPath()); });
    connect(&settings_, &core::AppSettings::whisperModelPathChanged, this,
            [this]() { controller_.setWhisperModelPath(settings_.whisperModelPath()); });
    connect(&settings_, &core::AppSettings::piperFileOutputExecutionEnabledChanged, this, [this]() {
        controller_.setPiperFileOutputExecutionEnabled(settings_.piperFileOutputExecutionEnabled());
    });
    connect(&settings_, &core::AppSettings::whisperTranscriptionExecutionEnabledChanged, this,
            [this]() {
                controller_.setWhisperTranscriptionExecutionEnabled(
                    settings_.whisperTranscriptionExecutionEnabled());
            });
    connect(&settings_, &core::AppSettings::soundEffectsEnabledChanged, this,
            &DesktopShellViewModel::soundEffectsEnabledChanged);
    connect(&settings_, &core::AppSettings::agentAutonomousModeChanged, this,
            [this]() { controller_.setAgentAutonomousMode(settings_.agentAutonomousMode()); });
    if (!controller_.isRemote()) {
        if (controller_.lmStudioEndpoint() != settings_.lmStudioEndpoint())
            controller_.setLmStudioEndpoint(settings_.lmStudioEndpoint());
        if (controller_.llamaCppEndpoint() != settings_.llamaCppEndpoint())
            controller_.setLlamaCppEndpoint(settings_.llamaCppEndpoint());
        controller_.setSelectedRuntimeProvider(settings_.selectedRuntimeProvider());
        controller_.setSelectedLocalModel(
            settings_.selectedModelForProvider(settings_.selectedRuntimeProvider()));
        controller_.setLocalChatInferenceEnabled(settings_.localChatInferenceEnabled());
        controller_.setLocalInferenceStreamingEnabled(settings_.localInferenceStreamingEnabled());
        controller_.setLocalInferenceTimeoutMs(settings_.localInferenceTimeoutMs());
        controller_.setLocalInferenceTemperature(settings_.localInferenceTemperature());
        controller_.setLocalInferenceTopP(settings_.localInferenceTopP());
        controller_.setLocalInferenceMaxTokens(settings_.localInferenceMaxTokens());
        controller_.setPromptContextInjectionEnabled(settings_.promptContextInjectionEnabled());
        controller_.setSemanticPromptInclusionEnabled(settings_.semanticPromptInclusionEnabled());
        controller_.setPiperBinaryPath(settings_.piperBinaryPath());
        controller_.setPiperModelPath(settings_.piperModelPath());
        controller_.setWhisperBinaryPath(settings_.whisperBinaryPath());
        controller_.setWhisperModelPath(settings_.whisperModelPath());
        controller_.configureSpeechTts(settings_.selectedTtsEngine(), settings_.kokoroModelPath(),
                                       settings_.kokoroVoice());
        controller_.setPiperFileOutputExecutionEnabled(settings_.piperFileOutputExecutionEnabled());
        controller_.setWhisperTranscriptionExecutionEnabled(
            settings_.whisperTranscriptionExecutionEnabled());
        controller_.setAgentAutonomousMode(settings_.agentAutonomousMode());
        controller_.configureWebSearch(settings_.webSearchProvider(), settings_.webSearchApiKey(),
                                       settings_.webSearchMaxResults());
        controller_.configureMcpServers(settings_.mcpServersJson());
        controller_.setSemanticProvider(settings_.semanticProvider(),
                                        settings_.semanticEmbeddingModel());
    }
    if (!controller_.isRemote() &&
        controller_.activeConversationId() != QStringLiteral("single-transcript")) {
        settings_.setActiveConversationId(controller_.activeConversationId());
    }

    // Pre-populate already existing notifications to avoid duplicate startup alerts
    const QString initialJson = settings_.notificationCenterJson();
    const QJsonDocument initialDoc = QJsonDocument::fromJson(initialJson.toUtf8());
    const QJsonArray initialArr =
        initialDoc.object().value(QStringLiteral("notifications")).toArray();
    for (const auto& val : initialArr) {
        notifiedIds_.insert(val.toObject().value(QStringLiteral("id")).toString());
    }

    connect(&settings_, &core::AppSettings::productExperienceChanged, this, [this]() {
        const QString json = settings_.notificationCenterJson();
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        const QJsonArray arr = doc.object().value(QStringLiteral("notifications")).toArray();
        for (const auto& val : arr) {
            const QJsonObject item = val.toObject();
            const QString id = item.value(QStringLiteral("id")).toString();
            const bool isRead = item.value(QStringLiteral("read")).toBool();
            const bool isArchived = item.value(QStringLiteral("archived")).toBool();
            if (!isRead && !isArchived && !item.value(QStringLiteral("snoozed")).toBool() && !notifiedIds_.contains(id)) {
                notifiedIds_.insert(id);
                QString title = item.value(QStringLiteral("title")).toString();
                const QString body = item.value(QStringLiteral("body")).toString();
                if (title.isEmpty()) {
                    title = QStringLiteral("Sentinel Assistant");
                }

                if (!shouldShowNotification(item.toVariantMap())) continue;
                // Send native system notification
                const QString policy = settings_.notificationPolicy();
                if (policy != QLatin1String("None") && policy != QLatin1String("Disabled") &&
                    QSystemTrayIcon::isSystemTrayAvailable()) {
                    const QString category = item.value(QStringLiteral("category")).toString();

                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (category == lastNotifiedCategory_ && (now - lastNotificationTime_) < 5000) {
                        continue;
                    }

                    lastNotifiedCategory_ = category;
                    lastNotificationTime_ = now;

                    if (controller_.isRemote()) {
                        emit nativeNotificationRequested(title, body, category);
                        continue;
                    }
                    if (!trayIcon_) {
                        trayIcon_ = new QSystemTrayIcon(this);
                        QIcon appIcon(QStringLiteral(":/branding/tray.png"));
#if defined(Q_OS_MACOS)
                        appIcon.setIsMask(true);
#endif
                        trayIcon_->setIcon(appIcon);
                        trayIcon_->show();

                        connect(trayIcon_, &QSystemTrayIcon::messageClicked, this, [this]() {
                            QString page = QStringLiteral("Dashboard");
                            if (lastNotifiedCategory_ == QLatin1String("Models")) {
                                page = QStringLiteral("Models");
                            } else if (lastNotifiedCategory_ == QLatin1String("Updates")) {
                                page = QStringLiteral("Settings");
                            }
                            emit requestWindowActive(page);
                        });
                    }
                    trayIcon_->showMessage(title, body, QSystemTrayIcon::NoIcon, 10000);
                }
            }
        }
    });

    setupBackgroundUpdateCheck();
}

QVariantList DesktopShellViewModel::persistentPermissionGrants() const {
    return controller_.persistentPermissionGrants();
}

bool DesktopShellViewModel::revokePersistentPermission(const QString& id) {
    const bool removed = controller_.revokePersistentPermission(id);
    if (removed)
        emit persistentPermissionGrantsChanged();
    return removed;
}

bool DesktopShellViewModel::clearPersistentPermissions() {
    const bool cleared = controller_.clearPersistentPermissions();
    if (cleared)
        emit persistentPermissionGrantsChanged();
    return cleared;
}

QString DesktopShellViewModel::providerName() const {
    return controller_.providerName();
}

QString DesktopShellViewModel::providerStatus() const {
    return controller_.providerStatus();
}

QString DesktopShellViewModel::agentStatus() const {
    return controller_.agentStatus();
}

bool DesktopShellViewModel::agentLoopActive() const {
    return controller_.agentLoopActive();
}

bool DesktopShellViewModel::agentAwaitingApproval() const {
    return controller_.agentAwaitingApproval();
}

QString DesktopShellViewModel::lastAgentResponse() const {
    return controller_.lastAgentResponse();
}

QString DesktopShellViewModel::latestToolPlanStatus() const {
    return controller_.latestToolPlanStatus();
}

QString DesktopShellViewModel::latestToolPlanSummary() const {
    return controller_.latestToolPlanSummary();
}

QString DesktopShellViewModel::latestApprovalStatus() const {
    return controller_.latestApprovalStatus();
}

QString DesktopShellViewModel::latestApprovalSummary() const {
    return controller_.latestApprovalSummary();
}

QString DesktopShellViewModel::latestSandboxStatus() const {
    return controller_.latestSandboxStatus();
}

QString DesktopShellViewModel::latestSandboxSummary() const {
    return controller_.latestSandboxSummary();
}

QString DesktopShellViewModel::latestToolExecutionStatus() const {
    return controller_.latestToolExecutionStatus();
}

QString DesktopShellViewModel::latestToolExecutionSummary() const {
    return controller_.latestToolExecutionSummary();
}

QString DesktopShellViewModel::latestAgentPipelineStatus() const {
    return controller_.latestAgentPipelineStatus();
}

QString DesktopShellViewModel::latestAgentPipelineSummary() const {
    return controller_.latestAgentPipelineSummary();
}

QString DesktopShellViewModel::runtimeSessionId() const {
    return controller_.runtimeSessionId();
}

QString DesktopShellViewModel::runtimeContextStatus() const {
    return controller_.runtimeContextStatus();
}

QString DesktopShellViewModel::runtimeContextSummary() const {
    return controller_.runtimeContextSummary();
}

QStringList DesktopShellViewModel::runtimeContextActiveToolIds() const {
    return controller_.runtimeContextActiveToolIds();
}

QString DesktopShellViewModel::conversationSessionId() const {
    return controller_.conversationSessionId();
}

QString DesktopShellViewModel::conversationSessionStatus() const {
    return controller_.conversationSessionStatus();
}

QString DesktopShellViewModel::interactionMode() const {
    return controller_.interactionMode();
}

QString DesktopShellViewModel::attentionState() const {
    return controller_.attentionState();
}

QString DesktopShellViewModel::contextWindowSummary() const {
    return controller_.contextWindowSummary();
}

QString DesktopShellViewModel::conversationState() const {
    return controller_.conversationState();
}

QString DesktopShellViewModel::conversationTransitionStatus() const {
    return controller_.conversationTransitionStatus();
}

QString DesktopShellViewModel::conversationTransitionSummary() const {
    return controller_.conversationTransitionSummary();
}

QString DesktopShellViewModel::conversationRuntimeSummary() const {
    return controller_.conversationRuntimeSummary();
}

QStringList DesktopShellViewModel::conversationRuntimeSummaryLines() const {
    return controller_.conversationRuntimeSummaryLines();
}

QString DesktopShellViewModel::conversationRuntimeRequestId() const {
    return controller_.conversationRuntimeRequestId();
}

QString DesktopShellViewModel::conversationRuntimeActiveModel() const {
    return controller_.conversationRuntimeActiveModel();
}

QString DesktopShellViewModel::conversationRuntimeActiveRoute() const {
    return controller_.conversationRuntimeActiveRoute();
}

bool DesktopShellViewModel::conversationRuntimeStreaming() const {
    return controller_.conversationRuntimeStreaming();
}

QString DesktopShellViewModel::conversationRuntimeLastSuccessSummary() const {
    return controller_.conversationRuntimeLastSuccessSummary();
}

QString DesktopShellViewModel::conversationRuntimeLastErrorSummary() const {
    return controller_.conversationRuntimeLastErrorSummary();
}

QString DesktopShellViewModel::conversationRuntimeLastLatencySummary() const {
    return controller_.conversationRuntimeLastLatencySummary();
}

int DesktopShellViewModel::agentActivityCount() const {
    return controller_.agentActivityCount();
}

QString DesktopShellViewModel::latestAgentActivitySummary() const {
    return controller_.latestAgentActivitySummary();
}

QString DesktopShellViewModel::currentRoutingMode() const {
    return controller_.currentRoutingMode();
}

void DesktopShellViewModel::setRoutingModeByName(const QString& routingModeName) {
    settings_.setRoutingModeName(routingModeName);
    controller_.setRoutingModeByName(settings_.routingModeName());
    emit modelRoutingChanged();
    emit conversationSessionChanged();
    emit orchestrationSnapshotChanged();
}

QStringList DesktopShellViewModel::availableRoutingModes() const {
    return settings_.availableRoutingModes();
}

QString DesktopShellViewModel::modelRoutingStatus() const {
    return controller_.modelRoutingStatus();
}

QString DesktopShellViewModel::selectedModelProviderSummary() const {
    return controller_.selectedModelProviderSummary();
}

QString DesktopShellViewModel::latestTaskPlanStatus() const {
    return controller_.latestTaskPlanStatus();
}

QString DesktopShellViewModel::latestTaskPlanSummary() const {
    return controller_.latestTaskPlanSummary();
}

int DesktopShellViewModel::plannedTaskStepCount() const {
    return controller_.plannedTaskStepCount();
}

int DesktopShellViewModel::registeredAgentCount() const {
    return controller_.registeredAgentCount();
}

QStringList DesktopShellViewModel::activeAgentSummaries() const {
    return controller_.activeAgentSummaries();
}

QString DesktopShellViewModel::currentAgentSummary() const {
    return controller_.currentAgentSummary();
}

QString DesktopShellViewModel::currentMemoryAffinitySummary() const {
    return controller_.currentMemoryAffinitySummary();
}

int DesktopShellViewModel::providerCatalogCount() const {
    return controller_.providerCatalogCount();
}

QStringList DesktopShellViewModel::providerCatalogSummaries() const {
    return controller_.providerCatalogSummaries();
}

int DesktopShellViewModel::memoryCatalogCount() const {
    return controller_.memoryCatalogCount();
}

QStringList DesktopShellViewModel::memoryCatalogSummaries() const {
    return controller_.memoryCatalogSummaries();
}

QString DesktopShellViewModel::orchestrationSnapshotStatus() const {
    return controller_.orchestrationSnapshotStatus();
}

QString DesktopShellViewModel::orchestrationSnapshotSummary() const {
    return controller_.orchestrationSnapshotSummary();
}

QStringList DesktopShellViewModel::orchestrationSignals() const {
    return controller_.orchestrationSignals();
}

QString DesktopShellViewModel::orchestrationReadinessStatus() const {
    return controller_.orchestrationReadinessStatus();
}

QString DesktopShellViewModel::orchestrationReadinessSummary() const {
    return controller_.orchestrationReadinessSummary();
}

QStringList DesktopShellViewModel::orchestrationDiagnostics() const {
    return controller_.orchestrationDiagnostics();
}

QString DesktopShellViewModel::agentTaskRuntimeStatus() const {
    return controller_.agentTaskRuntimeStatus();
}

QString DesktopShellViewModel::agentTaskRuntimeSummary() const {
    return controller_.agentTaskRuntimeSummary();
}

int DesktopShellViewModel::agentTaskRuntimeTaskCount() const {
    return controller_.agentTaskRuntimeTaskCount();
}

int DesktopShellViewModel::agentTaskQueueCount() const {
    return controller_.agentTaskQueueCount();
}

int DesktopShellViewModel::agentTaskQueueActiveCount() const {
    return controller_.agentTaskQueueActiveCount();
}

int DesktopShellViewModel::agentTaskQueuePlannedCount() const {
    return controller_.agentTaskQueuePlannedCount();
}

int DesktopShellViewModel::agentTaskQueueBlockedCount() const {
    return controller_.agentTaskQueueBlockedCount();
}

int DesktopShellViewModel::agentTaskQueueCompletedCount() const {
    return controller_.agentTaskQueueCompletedCount();
}

int DesktopShellViewModel::agentTaskQueueRefusedCount() const {
    return controller_.agentTaskQueueRefusedCount();
}

QString DesktopShellViewModel::latestAgentTaskSummary() const {
    return controller_.latestAgentTaskSummary();
}

QString DesktopShellViewModel::latestAgentTaskLifecycleSummary() const {
    return controller_.latestAgentTaskLifecycleSummary();
}

QStringList DesktopShellViewModel::agentTaskQueueSummaries() const {
    return controller_.agentTaskQueueSummaries();
}

QStringList DesktopShellViewModel::agentTaskTraceSummaries() const {
    return controller_.agentTaskTraceSummaries();
}

QString DesktopShellViewModel::agentPlanningSessionStatus() const {
    return controller_.agentPlanningSessionStatus();
}

QString DesktopShellViewModel::agentPlanningSessionSummary() const {
    return controller_.agentPlanningSessionSummary();
}

int DesktopShellViewModel::agentPlanningCandidateCount() const {
    return controller_.agentPlanningCandidateCount();
}

int DesktopShellViewModel::agentPlanningRefusedCount() const {
    return controller_.agentPlanningRefusedCount();
}

QStringList DesktopShellViewModel::agentPlanningCandidateSummaries() const {
    return controller_.agentPlanningCandidateSummaries();
}

QStringList DesktopShellViewModel::agentPlanningArbitrationSummaries() const {
    return controller_.agentPlanningArbitrationSummaries();
}

QStringList DesktopShellViewModel::agentPlanningRefusalSummaries() const {
    return controller_.agentPlanningRefusalSummaries();
}

QString DesktopShellViewModel::agentPlanningFallbackSummary() const {
    return controller_.agentPlanningFallbackSummary();
}

QString DesktopShellViewModel::agentCapabilityRegistryStatus() const {
    return controller_.agentCapabilityRegistryStatus();
}

QString DesktopShellViewModel::agentCapabilityRegistrySummary() const {
    return controller_.agentCapabilityRegistrySummary();
}

int DesktopShellViewModel::agentCapabilityCount() const {
    return controller_.agentCapabilityCount();
}

int DesktopShellViewModel::agentCapabilityEnabledCount() const {
    return controller_.agentCapabilityEnabledCount();
}

int DesktopShellViewModel::agentCapabilityDisabledCount() const {
    return controller_.agentCapabilityDisabledCount();
}

int DesktopShellViewModel::agentCapabilityRestrictedCount() const {
    return controller_.agentCapabilityRestrictedCount();
}

QStringList DesktopShellViewModel::agentCapabilitySummaries() const {
    return controller_.agentCapabilitySummaries();
}

QStringList DesktopShellViewModel::agentCapabilityReadinessSummaries() const {
    return controller_.agentCapabilityReadinessSummaries();
}

QStringList DesktopShellViewModel::agentCapabilitySafetySummaries() const {
    return controller_.agentCapabilitySafetySummaries();
}

QString DesktopShellViewModel::toolContractRegistryStatus() const {
    return controller_.toolContractRegistryStatus();
}

QString DesktopShellViewModel::toolContractRegistrySummary() const {
    return controller_.toolContractRegistrySummary();
}

int DesktopShellViewModel::toolContractCount() const {
    return controller_.toolContractCount();
}

int DesktopShellViewModel::toolContractEnabledCount() const {
    return controller_.toolContractEnabledCount();
}

int DesktopShellViewModel::toolContractDisabledCount() const {
    return controller_.toolContractDisabledCount();
}

int DesktopShellViewModel::toolContractRestrictedCount() const {
    return controller_.toolContractRestrictedCount();
}

QStringList DesktopShellViewModel::toolContractSummaries() const {
    return controller_.toolContractSummaries();
}

QStringList DesktopShellViewModel::toolContractPermissionSummaries() const {
    return controller_.toolContractPermissionSummaries();
}

QStringList DesktopShellViewModel::toolContractSandboxSummaries() const {
    return controller_.toolContractSandboxSummaries();
}

QStringList DesktopShellViewModel::toolContractReadinessSummaries() const {
    return controller_.toolContractReadinessSummaries();
}

QStringList DesktopShellViewModel::toolContractSafetySummaries() const {
    return controller_.toolContractSafetySummaries();
}

QString DesktopShellViewModel::localRuntimeStatus() const {
    return controller_.localRuntimeStatus();
}

QString DesktopShellViewModel::localRuntimeHealth() const {
    return controller_.localRuntimeHealth();
}

QString DesktopShellViewModel::localRuntimeSummary() const {
    return controller_.localRuntimeSummary();
}

QStringList DesktopShellViewModel::localRuntimeCapabilities() const {
    return controller_.localRuntimeCapabilities();
}

QString DesktopShellViewModel::localRuntimeResponseStatus() const {
    return controller_.localRuntimeResponseStatus();
}

QString DesktopShellViewModel::localRuntimeResponseSummary() const {
    return controller_.localRuntimeResponseSummary();
}

int DesktopShellViewModel::localRuntimeSessionCount() const {
    return controller_.localRuntimeSessionCount();
}

QString DesktopShellViewModel::localRuntimeSessionStatus() const {
    return controller_.localRuntimeSessionStatus();
}

QString DesktopShellViewModel::localRuntimeSessionHealth() const {
    return controller_.localRuntimeSessionHealth();
}

QString DesktopShellViewModel::localRuntimeSessionSummary() const {
    return controller_.localRuntimeSessionSummary();
}

QString DesktopShellViewModel::localRuntimeAllocationSummary() const {
    return controller_.localRuntimeAllocationSummary();
}

QString DesktopShellViewModel::localRuntimeReservationSummary() const {
    return controller_.localRuntimeReservationSummary();
}

QStringList DesktopShellViewModel::localRuntimeSessionSummaries() const {
    return controller_.localRuntimeSessionSummaries();
}

int DesktopShellViewModel::runtimeCapabilityCount() const {
    return controller_.runtimeCapabilityCount();
}

QStringList DesktopShellViewModel::enabledRuntimeCapabilitySummaries() const {
    return controller_.enabledRuntimeCapabilitySummaries();
}

QStringList DesktopShellViewModel::disabledRuntimeCapabilitySummaries() const {
    return controller_.disabledRuntimeCapabilitySummaries();
}

QString DesktopShellViewModel::runtimeNegotiationProfileSummary() const {
    return controller_.runtimeNegotiationProfileSummary();
}

QString DesktopShellViewModel::runtimeNegotiationSummary() const {
    return controller_.runtimeNegotiationSummary();
}

QString DesktopShellViewModel::localOnlyRuntimeEnforcementSummary() const {
    return controller_.localOnlyRuntimeEnforcementSummary();
}

QString DesktopShellViewModel::runtimePermissionDecision() const {
    return controller_.runtimePermissionDecision();
}

QString DesktopShellViewModel::runtimePermissionSummary() const {
    return controller_.runtimePermissionSummary();
}

QString DesktopShellViewModel::runtimeSafetyDecision() const {
    return controller_.runtimeSafetyDecision();
}

QString DesktopShellViewModel::runtimeSafetySummary() const {
    return controller_.runtimeSafetySummary();
}

QString DesktopShellViewModel::runtimePipelineStatus() const {
    return controller_.runtimePipelineStatus();
}

QString DesktopShellViewModel::runtimePipelineSummary() const {
    return controller_.runtimePipelineSummary();
}

QStringList DesktopShellViewModel::runtimePipelineTraceSummaries() const {
    return controller_.runtimePipelineTraceSummaries();
}

QString DesktopShellViewModel::executionLifecycleState() const {
    return controller_.executionLifecycleState();
}

QString DesktopShellViewModel::executionLifecycleStatus() const {
    return controller_.executionLifecycleStatus();
}

QString DesktopShellViewModel::executionLifecycleSummary() const {
    return controller_.executionLifecycleSummary();
}

QStringList DesktopShellViewModel::executionLifecycleTraceSummaries() const {
    return controller_.executionLifecycleTraceSummaries();
}

QString DesktopShellViewModel::executionSessionId() const {
    return controller_.executionSessionId();
}

QString DesktopShellViewModel::executionSessionStatus() const {
    return controller_.executionSessionStatus();
}

QString DesktopShellViewModel::executionSessionOwnership() const {
    return controller_.executionSessionOwnership();
}

QString DesktopShellViewModel::executionCoordinationMode() const {
    return controller_.executionCoordinationMode();
}

QString DesktopShellViewModel::executionSessionSummary() const {
    return controller_.executionSessionSummary();
}

QString DesktopShellViewModel::executionCoordinationSnapshotSummary() const {
    return controller_.executionCoordinationSnapshotSummary();
}

QString DesktopShellViewModel::localRuntimeAdapterStatus() const {
    return controller_.localRuntimeAdapterStatus();
}

QString DesktopShellViewModel::localRuntimeAdapterHealth() const {
    return controller_.localRuntimeAdapterHealth();
}

QString DesktopShellViewModel::localRuntimeAdapterSummary() const {
    return controller_.localRuntimeAdapterSummary();
}

QStringList DesktopShellViewModel::localRuntimeAdapterCapabilitySummaries() const {
    return controller_.localRuntimeAdapterCapabilitySummaries();
}

QString DesktopShellViewModel::providerRuntimeBridgeStatus() const {
    return controller_.providerRuntimeBridgeStatus();
}

QString DesktopShellViewModel::providerRuntimeBridgeSummary() const {
    return controller_.providerRuntimeBridgeSummary();
}

QString DesktopShellViewModel::providerRuntimeBridgeResponseSummary() const {
    return controller_.providerRuntimeBridgeResponseSummary();
}

QString DesktopShellViewModel::runtimeIntegrationReadinessStatus() const {
    return controller_.runtimeIntegrationReadinessStatus();
}

QString DesktopShellViewModel::runtimeIntegrationReadinessSummary() const {
    return controller_.runtimeIntegrationReadinessSummary();
}

QStringList DesktopShellViewModel::runtimeIntegrationReadinessChecks() const {
    return controller_.runtimeIntegrationReadinessChecks();
}

QString DesktopShellViewModel::selectedRuntimeProvider() const {
    if (controller_.isRemote())
        return controller_.selectedRuntimeProvider();
    return settings_.selectedRuntimeProvider();
}

void DesktopShellViewModel::setSelectedRuntimeProvider(const QString& providerId) {
    controller_.setSelectedRuntimeProvider(providerId);
    if (controller_.isRemote())
        return;
    const auto providerModel =
        settings_.selectedModelForProvider(controller_.selectedRuntimeProvider());
    if (controller_.selectedLocalModel() != providerModel)
        controller_.setSelectedLocalModel(providerModel);
}

QString DesktopShellViewModel::activeRuntimeProviderId() const {
    return controller_.activeRuntimeProviderId();
}

QString DesktopShellViewModel::activeRuntimeProviderLabel() const {
    return controller_.activeRuntimeProviderLabel();
}

QString DesktopShellViewModel::activeRuntimeModelLabel() const {
    return controller_.activeRuntimeModelLabel();
}

QString DesktopShellViewModel::activeRuntimeReadinessState() const {
    return controller_.activeRuntimeReadinessState();
}

QString DesktopShellViewModel::activeRuntimeReadinessSummary() const {
    return controller_.activeRuntimeReadinessSummary();
}

QString DesktopShellViewModel::activeRuntimeLocalOnlySummary() const {
    return controller_.activeRuntimeLocalOnlySummary();
}

QStringList DesktopShellViewModel::selectableRuntimeProviderIds() const {
    return controller_.selectableRuntimeProviderIds();
}

QStringList DesktopShellViewModel::selectableRuntimeProviderLabels() const {
    return controller_.selectableRuntimeProviderLabels();
}

QStringList DesktopShellViewModel::runtimeProviderCardSummaries() const {
    return controller_.runtimeProviderCardSummaries();
}

QStringList DesktopShellViewModel::runtimeProviderCapabilitySummaries() const {
    return controller_.runtimeProviderCapabilitySummaries();
}

QStringList DesktopShellViewModel::runtimeProviderValidationTraces() const {
    return controller_.runtimeProviderValidationTraces();
}

QStringList DesktopShellViewModel::installedRuntimeProviderSummaries() const {
    return controller_.installedRuntimeProviderSummaries();
}

QStringList DesktopShellViewModel::configuredRuntimeProviderSummaries() const {
    return controller_.configuredRuntimeProviderSummaries();
}

QStringList DesktopShellViewModel::availableLocalRuntimeSummaries() const {
    return controller_.availableLocalRuntimeSummaries();
}

QString DesktopShellViewModel::providerCredentialRegistryStatus() const {
    return controller_.providerCredentialRegistryStatus();
}

QString DesktopShellViewModel::providerCredentialRegistrySummary() const {
    return controller_.providerCredentialRegistrySummary();
}

QStringList DesktopShellViewModel::providerCredentialSummaries() const {
    return controller_.providerCredentialSummaries();
}

QStringList DesktopShellViewModel::providerCredentialReadinessSummaries() const {
    return controller_.providerCredentialReadinessSummaries();
}

QStringList DesktopShellViewModel::providerCredentialSafetySummaries() const {
    return controller_.providerCredentialSafetySummaries();
}

QString DesktopShellViewModel::credentialStoreSummary() const {
    return controller_.credentialStoreSummary();
}

QString DesktopShellViewModel::credentialStoreBackendSummary() const {
    return controller_.credentialStoreBackendSummary();
}

QString DesktopShellViewModel::credentialStoreSafetySummary() const {
    return controller_.credentialStoreSafetySummary();
}

QStringList DesktopShellViewModel::credentialStoreTraceSummaries() const {
    return controller_.credentialStoreTraceSummaries();
}

QString DesktopShellViewModel::credentialActionReadiness() const {
    return controller_.credentialActionReadiness();
}

QString DesktopShellViewModel::credentialExecutionStatus() const {
    return controller_.credentialExecutionStatus();
}

QString DesktopShellViewModel::ollamaEndpoint() const {
    return controller_.ollamaEndpoint();
}

void DesktopShellViewModel::setOllamaEndpoint(const QString& endpoint) {
    const auto result = setProductSetting(QStringLiteral("models.ollama-endpoint"), endpoint);
    if (result.value("accepted").toBool() &&
        controller_.ollamaEndpoint() != settings_.ollamaEndpoint())
        controller_.setOllamaEndpoint(settings_.ollamaEndpoint());
}

QString DesktopShellViewModel::lmStudioEndpoint() const {
    return settings_.lmStudioEndpoint();
}

void DesktopShellViewModel::setLmStudioEndpoint(const QString& endpoint) {
    const auto result = setProductSetting(QStringLiteral("models.lm-studio-endpoint"), endpoint);
    if (result.value("accepted").toBool())
        controller_.setLmStudioEndpoint(settings_.lmStudioEndpoint());
}

QString DesktopShellViewModel::llamaCppEndpoint() const {
    return settings_.llamaCppEndpoint();
}

void DesktopShellViewModel::setLlamaCppEndpoint(const QString& endpoint) {
    const auto result = setProductSetting(QStringLiteral("models.llama-cpp-endpoint"), endpoint);
    if (result.value("accepted").toBool())
        controller_.setLlamaCppEndpoint(settings_.llamaCppEndpoint());
}

QString DesktopShellViewModel::cloudApiEndpoint() const {
    return settings_.cloudApiEndpoint();
}

void DesktopShellViewModel::setCloudApiEndpoint(const QString& endpoint) {
    if (settings_.cloudApiEndpoint() != endpoint) {
        settings_.setCloudApiEndpoint(endpoint);
    }
}

QString DesktopShellViewModel::ollamaConnectionStatus() const {
    return controller_.ollamaConnectionStatus();
}

QString DesktopShellViewModel::ollamaHealthStatus() const {
    return controller_.ollamaHealthStatus();
}

QString DesktopShellViewModel::ollamaHealthSummary() const {
    return controller_.ollamaHealthSummary();
}

QString DesktopShellViewModel::ollamaDiscoveryStatus() const {
    return controller_.ollamaDiscoveryStatus();
}

QString DesktopShellViewModel::localInferenceHealthSummary() const {
    return ollamaHealthSummary();
}

QString DesktopShellViewModel::activeLocalModelName() const {
    const auto selected = selectedLocalModel();
    return !selected.isEmpty() ? selected : activeRuntimeModelLabel();
}

QString DesktopShellViewModel::selectedOllamaModel() const {
    return selectedLocalModel();
}

void DesktopShellViewModel::setSelectedOllamaModel(const QString& modelName) {
    setSelectedLocalModel(modelName);
}

bool DesktopShellViewModel::proxyEnabled() const {
    return settings_.proxyEnabled();
}

void DesktopShellViewModel::setProxyEnabled(bool enabled) {
    settings_.setProxyEnabled(enabled);
}

QString DesktopShellViewModel::proxyType() const {
    return settings_.proxyType();
}

void DesktopShellViewModel::setProxyType(const QString& type) {
    settings_.setProxyType(type);
}

QString DesktopShellViewModel::proxyHost() const {
    return settings_.proxyHost();
}

void DesktopShellViewModel::setProxyHost(const QString& host) {
    settings_.setProxyHost(host);
}

int DesktopShellViewModel::proxyPort() const {
    return static_cast<int>(settings_.proxyPort());
}

void DesktopShellViewModel::setProxyPort(int port) {
    settings_.setProxyPort(static_cast<quint16>(qMax(0, port)));
}

QString DesktopShellViewModel::proxyUser() const {
    return settings_.proxyUser();
}

void DesktopShellViewModel::setProxyUser(const QString& user) {
    settings_.setProxyUser(user);
}

QString DesktopShellViewModel::proxyPassword() const {
    return {};
}

bool DesktopShellViewModel::proxyPasswordConfigured() const {
    return !settings_.proxyPassword().isEmpty();
}

void DesktopShellViewModel::setProxyPassword(const QString& password) {
    settings_.setProxyPassword(password);
}

int DesktopShellViewModel::ollamaModelCount() const {
    return controller_.ollamaModelCount();
}

bool DesktopShellViewModel::isOnline() const {
    const auto* ni = QNetworkInformation::instance();
    return !ni || ni->reachability() == QNetworkInformation::Reachability::Online;
}

QStringList DesktopShellViewModel::ollamaModelNames() const {
    return controller_.ollamaModelNames();
}

QString DesktopShellViewModel::cloudModelDiscoveryError() const {
    return controller_.cloudModelDiscoveryError();
}

QStringList DesktopShellViewModel::installedOllamaModelNames() const {
    return controller_.installedOllamaModelNames();
}

QStringList DesktopShellViewModel::loadedLMStudioModelNames() const {
    return controller_.loadedLMStudioModelNames();
}

QStringList DesktopShellViewModel::ollamaModelSummaries() const {
    return controller_.ollamaModelSummaries();
}

QString DesktopShellViewModel::selectedLocalModel() const {
    return controller_.selectedLocalModel();
}

void DesktopShellViewModel::setSelectedLocalModel(const QString& model) {
    controller_.setSelectedLocalModel(model);
}

QString DesktopShellViewModel::selectedLocalModelStatus() const {
    return controller_.selectedLocalModelStatus();
}

QString DesktopShellViewModel::selectedLocalModelSummary() const {
    return controller_.selectedLocalModelSummary();
}

QString DesktopShellViewModel::selectedLocalModelMetadataSummary() const {
    return controller_.selectedLocalModelMetadataSummary();
}

QString DesktopShellViewModel::activeLocalRuntimeBadge() const {
    return controller_.activeLocalRuntimeBadge();
}

QString DesktopShellViewModel::modelRegistryStatus() const {
    return controller_.modelRegistryStatus();
}

QString DesktopShellViewModel::modelRegistrySummary() const {
    return controller_.modelRegistrySummary();
}

QStringList DesktopShellViewModel::modelRegistryModelSummaries() const {
    return controller_.modelRegistryModelSummaries();
}

QStringList DesktopShellViewModel::modelLibraryInstalledSummaries() const {
    return controller_.modelLibraryInstalledSummaries();
}

QStringList DesktopShellViewModel::modelLibraryAvailableSummaries() const {
    return controller_.modelLibraryAvailableSummaries();
}

QStringList DesktopShellViewModel::modelLibraryRecommendedSummaries() const {
    return controller_.modelLibraryRecommendedSummaries();
}

QStringList DesktopShellViewModel::modelLibraryDetailSummaries() const {
    return controller_.modelLibraryDetailSummaries();
}

QStringList DesktopShellViewModel::providerDiscoverySummaries() const {
    return controller_.providerDiscoverySummaries();
}

QStringList DesktopShellViewModel::modelRoleIds() const {
    return core::modelRoleIds();
}

QStringList DesktopShellViewModel::modelRoleAssignmentSummaries() const {
    QStringList summaries;
    const QList<core::ModelRole> roles{
        core::ModelRole::PrimaryChat, core::ModelRole::Coding, core::ModelRole::Summarizer,
        core::ModelRole::Research,    core::ModelRole::Fast,   core::ModelRole::Voice,
        core::ModelRole::Embedding,
    };
    for (const auto role : roles) {
        const auto roleId = core::modelRoleId(role);
        const auto selected = settings_.selectedModelForRole(roleId);
        summaries.append(
            QStringLiteral("%1 - %2 - routing metadata only; automatic multi-model "
                           "execution is disabled.")
                .arg(core::modelRoleDisplayName(role),
                     selected.isEmpty() ? QStringLiteral("No model assigned") : selected));
    }
    return summaries;
}

void DesktopShellViewModel::assignModelRole(const QString& roleId, const QString& modelId) {
    settings_.setSelectedModelForRole(roleId, modelId);
}

QStringList DesktopShellViewModel::modelAdvisorRecommendationSummaries() const {
    return controller_.modelAdvisorRecommendationSummaries();
}

QStringList DesktopShellViewModel::modelAdvisorAvoidSummaries() const {
    return controller_.modelAdvisorAvoidSummaries();
}

QStringList DesktopShellViewModel::downloadsCenterSummaries() const {
    return controller_.downloadsCenterSummaries();
}

QStringList DesktopShellViewModel::benchmarkHubSummaries() const {
    return controller_.benchmarkHubSummaries();
}

QStringList DesktopShellViewModel::selectedModelCapabilityLabels() const {
    return controller_.selectedModelCapabilityLabels();
}

QVariantList DesktopShellViewModel::modelCapabilitySettings() const {
    return controller_.modelCapabilitySettings();
}

QString DesktopShellViewModel::modelCapabilityCompatibilitySummary() const {
    return controller_.modelCapabilityCompatibilitySummary();
}

bool DesktopShellViewModel::setModelCapabilityOverride(const QString& capabilityId,
                                                       const QString& value) {
    return controller_.setModelCapabilityOverride(capabilityId, value);
}

bool DesktopShellViewModel::setModelCapabilityNumberOverride(const QString& capabilityId,
                                                             int value) {
    return controller_.setModelCapabilityNumberOverride(capabilityId, value);
}

void DesktopShellViewModel::resetModelCapabilityOverrides() {
    controller_.resetModelCapabilityOverrides();
}

QString DesktopShellViewModel::modelManagementStatus() const {
    return controller_.modelManagementStatus();
}

QString DesktopShellViewModel::modelManagementSummary() const {
    return controller_.modelManagementSummary();
}

QString DesktopShellViewModel::modelManagementActionAvailability() const {
    return controller_.modelManagementActionAvailability();
}

QStringList DesktopShellViewModel::modelRecommendationSummaries() const {
    return controller_.modelRecommendationSummaries();
}

QStringList DesktopShellViewModel::modelRequirementSummaries() const {
    return controller_.modelRequirementSummaries();
}

QString DesktopShellViewModel::voiceRuntimeMode() const {
    return controller_.voiceRuntimeMode();
}

bool DesktopShellViewModel::voiceEnabled() const {
    return controller_.voiceEnabled();
}

bool DesktopShellViewModel::voiceRecordingActive() const {
    return voiceRecordingActive_;
}

QString DesktopShellViewModel::voiceReadinessStatus() const {
    return controller_.voiceReadinessStatus();
}

QString DesktopShellViewModel::voiceReadinessSummary() const {
    return controller_.voiceReadinessSummary();
}

QStringList DesktopShellViewModel::voiceReadinessChecks() const {
    return controller_.voiceReadinessChecks();
}

QStringList DesktopShellViewModel::voiceCapabilitySummaries() const {
    return controller_.voiceCapabilitySummaries();
}

QString DesktopShellViewModel::textToSpeechStatus() const {
    return controller_.textToSpeechStatus();
}

QString DesktopShellViewModel::textToSpeechSummary() const {
    return controller_.textToSpeechSummary();
}

QString DesktopShellViewModel::speechToTextStatus() const {
    return controller_.speechToTextStatus();
}

QString DesktopShellViewModel::speechToTextSummary() const {
    return controller_.speechToTextSummary();
}

QString DesktopShellViewModel::voiceSessionId() const {
    return controller_.voiceSessionId();
}

QString DesktopShellViewModel::voiceSessionStatus() const {
    return controller_.voiceSessionStatus();
}

QString DesktopShellViewModel::voiceSessionSummary() const {
    return controller_.voiceSessionSummary();
}

QString DesktopShellViewModel::voicePipelineStatus() const {
    return controller_.voicePipelineStatus();
}

QString DesktopShellViewModel::voicePipelineSummary() const {
    return controller_.voicePipelineSummary();
}

QStringList DesktopShellViewModel::voicePipelineTraceSummaries() const {
    return controller_.voicePipelineTraceSummaries();
}

QString DesktopShellViewModel::voicePipelineSessionStatus() const {
    return controller_.voicePipelineSessionStatus();
}

QString DesktopShellViewModel::voicePipelineSessionSummary() const {
    return controller_.voicePipelineSessionSummary();
}

QStringList DesktopShellViewModel::voicePipelineSessionStageReadinessSummaries() const {
    return controller_.voicePipelineSessionStageReadinessSummaries();
}

QStringList DesktopShellViewModel::voicePipelineSessionTraceSummaries() const {
    return controller_.voicePipelineSessionTraceSummaries();
}

QString DesktopShellViewModel::voicePipelineSessionFallbackSummary() const {
    return controller_.voicePipelineSessionFallbackSummary();
}

QString DesktopShellViewModel::voicePipelineSessionSafetySummary() const {
    return controller_.voicePipelineSessionSafetySummary();
}

QStringList DesktopShellViewModel::voicePipelineSessionSafetyChecks() const {
    return controller_.voicePipelineSessionSafetyChecks();
}

int DesktopShellViewModel::voicePipelineSessionReadyStageCount() const {
    return controller_.voicePipelineSessionReadyStageCount();
}

int DesktopShellViewModel::voicePipelineSessionBlockedStageCount() const {
    return controller_.voicePipelineSessionBlockedStageCount();
}

QString DesktopShellViewModel::audioFileSessionStatus() const {
    return controller_.audioFileSessionStatus();
}

QString DesktopShellViewModel::audioFileSessionSummary() const {
    return controller_.audioFileSessionSummary();
}

QString DesktopShellViewModel::audioFileSessionReadinessSummary() const {
    return controller_.audioFileSessionReadinessSummary();
}

QStringList DesktopShellViewModel::audioFileValidationSummaries() const {
    return controller_.audioFileValidationSummaries();
}

QStringList DesktopShellViewModel::audioFileSupportedExtensionSummaries() const {
    return controller_.audioFileSupportedExtensionSummaries();
}

QString DesktopShellViewModel::audioFileSessionFallbackSummary() const {
    return controller_.audioFileSessionFallbackSummary();
}

QString DesktopShellViewModel::audioFileSessionSafetySummary() const {
    return controller_.audioFileSessionSafetySummary();
}

QStringList DesktopShellViewModel::audioFileSessionSafetyChecks() const {
    return controller_.audioFileSessionSafetyChecks();
}

QStringList DesktopShellViewModel::audioFileSessionRefusalSummaries() const {
    return controller_.audioFileSessionRefusalSummaries();
}

QStringList DesktopShellViewModel::audioFileTraceSummaries() const {
    return controller_.audioFileTraceSummaries();
}

QString DesktopShellViewModel::voiceRuntimeStatus() const {
    return controller_.voiceRuntimeStatus();
}

QString DesktopShellViewModel::voiceRuntimeSummary() const {
    return controller_.voiceRuntimeSummary();
}

QStringList DesktopShellViewModel::voiceRuntimeCheckSummaries() const {
    return controller_.voiceRuntimeCheckSummaries();
}

bool DesktopShellViewModel::voiceRuntimeAvailable() const {
    return controller_.voiceRuntimeAvailable();
}

bool DesktopShellViewModel::voiceTextToSpeechAvailable() const {
    return controller_.voiceTextToSpeechAvailable();
}

bool DesktopShellViewModel::voiceSpeechToTextAvailable() const {
    return controller_.voiceSpeechToTextAvailable();
}

bool DesktopShellViewModel::voiceMicrophoneEnabled() const {
    return controller_.voiceMicrophoneEnabled();
}

bool DesktopShellViewModel::voicePlaybackEnabled() const {
    return controller_.voicePlaybackEnabled();
}

bool DesktopShellViewModel::voiceLocalOnlyPolicy() const {
    return controller_.voiceLocalOnlyPolicy();
}

bool DesktopShellViewModel::voiceProcessExecutionEnabled() const {
    return controller_.voiceProcessExecutionEnabled();
}

QString DesktopShellViewModel::voiceRuntimeEnvironmentStatus() const {
    return controller_.voiceRuntimeEnvironmentStatus();
}

QString DesktopShellViewModel::voiceRuntimeEnvironmentSummary() const {
    return controller_.voiceRuntimeEnvironmentSummary();
}

QStringList DesktopShellViewModel::voiceBinarySummaries() const {
    return controller_.voiceBinarySummaries();
}

QStringList DesktopShellViewModel::voiceModelSummaries() const {
    return controller_.voiceModelSummaries();
}

QStringList DesktopShellViewModel::voiceRuntimePermissionSummaries() const {
    return controller_.voiceRuntimePermissionSummaries();
}

QString DesktopShellViewModel::voiceRuntimeSafetyStatus() const {
    return controller_.voiceRuntimeSafetyStatus();
}

QString DesktopShellViewModel::voiceRuntimeSafetySummary() const {
    return controller_.voiceRuntimeSafetySummary();
}

QStringList DesktopShellViewModel::voiceRuntimeSafetyChecks() const {
    return controller_.voiceRuntimeSafetyChecks();
}

bool DesktopShellViewModel::voiceRuntimeExecutionAllowed() const {
    return controller_.voiceRuntimeExecutionAllowed();
}

QString DesktopShellViewModel::piperTtsStatus() const {
    return controller_.piperTtsStatus();
}

QString DesktopShellViewModel::piperTtsSummary() const {
    return controller_.piperTtsSummary();
}

QStringList DesktopShellViewModel::piperTtsReadinessChecks() const {
    return controller_.piperTtsReadinessChecks();
}

bool DesktopShellViewModel::piperTtsReady() const {
    return controller_.piperTtsReady();
}

QString DesktopShellViewModel::piperTtsFileOutputStatus() const {
    return controller_.piperTtsFileOutputStatus();
}

QString DesktopShellViewModel::piperTtsFileOutputSummary() const {
    return controller_.piperTtsFileOutputSummary();
}

QString DesktopShellViewModel::piperSynthesisStatus() const {
    return controller_.piperSynthesisStatus();
}

QString DesktopShellViewModel::piperSynthesisReadinessSummary() const {
    return controller_.piperSynthesisReadinessSummary();
}

QString DesktopShellViewModel::piperSynthesisLastSummary() const {
    return controller_.piperSynthesisLastSummary();
}

QString DesktopShellViewModel::piperSynthesisFallbackSummary() const {
    return controller_.piperSynthesisFallbackSummary();
}

QString DesktopShellViewModel::piperSynthesisSafetySummary() const {
    return controller_.piperSynthesisSafetySummary();
}

QStringList DesktopShellViewModel::piperSynthesisTraceSummaries() const {
    return controller_.piperSynthesisTraceSummaries();
}

QString DesktopShellViewModel::piperBinaryPath() const {
    return controller_.piperBinaryPath();
}

void DesktopShellViewModel::setPiperBinaryPath(const QString& path) {
    settings_.setPiperBinaryPath(path);
    if (controller_.piperBinaryPath() != settings_.piperBinaryPath()) {
        controller_.setPiperBinaryPath(settings_.piperBinaryPath());
    }
}

QString DesktopShellViewModel::piperModelPath() const {
    return controller_.piperModelPath();
}

void DesktopShellViewModel::setPiperModelPath(const QString& path) {
    settings_.setPiperModelPath(path);
    if (controller_.piperModelPath() != settings_.piperModelPath()) {
        controller_.setPiperModelPath(settings_.piperModelPath());
    }
}

QString DesktopShellViewModel::selectedTtsEngine() const {
    return settings_.selectedTtsEngine();
}

void DesktopShellViewModel::setSelectedTtsEngine(const QString& engine) {
    settings_.setSelectedTtsEngine(engine);
    emit voiceConfigurationChanged();
}

QString DesktopShellViewModel::kokoroModelPath() const {
    return settings_.kokoroModelPath();
}

void DesktopShellViewModel::setKokoroModelPath(const QString& path) {
    settings_.setKokoroModelPath(path);
    emit voiceConfigurationChanged();
}

QString DesktopShellViewModel::kokoroVoice() const {
    return settings_.kokoroVoice();
}

void DesktopShellViewModel::setKokoroVoice(const QString& voice) {
    settings_.setKokoroVoice(voice);
    emit voiceConfigurationChanged();
}

QString DesktopShellViewModel::whisperBinaryPath() const {
    return controller_.whisperBinaryPath();
}

void DesktopShellViewModel::setWhisperBinaryPath(const QString& path) {
    settings_.setWhisperBinaryPath(path);
    if (controller_.whisperBinaryPath() != settings_.whisperBinaryPath()) {
        controller_.setWhisperBinaryPath(settings_.whisperBinaryPath());
    }
}

QString DesktopShellViewModel::whisperModelPath() const {
    return controller_.whisperModelPath();
}

void DesktopShellViewModel::setWhisperModelPath(const QString& path) {
    settings_.setWhisperModelPath(path);
    if (controller_.whisperModelPath() != settings_.whisperModelPath()) {
        controller_.setWhisperModelPath(settings_.whisperModelPath());
    }
}

QStringList DesktopShellViewModel::voiceConfigurationSummaries() const {
    return controller_.voiceConfigurationSummaries();
}

QString DesktopShellViewModel::voiceConfigurationReadinessSummary() const {
    return controller_.voiceConfigurationReadinessSummary();
}

QStringList DesktopShellViewModel::voiceConfigurationStatusBadges() const {
    return controller_.voiceConfigurationStatusBadges();
}

QStringList DesktopShellViewModel::voiceConfigurationHintSummaries() const {
    return controller_.voiceConfigurationHintSummaries();
}

QStringList DesktopShellViewModel::voiceConfigurationValidationSummaries() const {
    return controller_.voiceConfigurationValidationSummaries();
}

QString DesktopShellViewModel::piperFileOutputReadinessStatus() const {
    return controller_.piperFileOutputReadinessStatus();
}

QString DesktopShellViewModel::piperFileOutputReadinessSummary() const {
    return controller_.piperFileOutputReadinessSummary();
}

bool DesktopShellViewModel::piperFileOutputExecutionEnabled() const {
    return controller_.piperFileOutputExecutionEnabled();
}

void DesktopShellViewModel::setPiperFileOutputExecutionEnabled(bool enabled) {
    settings_.setPiperFileOutputExecutionEnabled(enabled);
    if (controller_.piperFileOutputExecutionEnabled() !=
        settings_.piperFileOutputExecutionEnabled()) {
        controller_.setPiperFileOutputExecutionEnabled(settings_.piperFileOutputExecutionEnabled());
    }
}

bool DesktopShellViewModel::whisperTranscriptionExecutionEnabled() const {
    return controller_.whisperTranscriptionExecutionEnabled();
}

void DesktopShellViewModel::setWhisperTranscriptionExecutionEnabled(bool enabled) {
    settings_.setWhisperTranscriptionExecutionEnabled(enabled);
    controller_.setWhisperTranscriptionExecutionEnabled(
        settings_.whisperTranscriptionExecutionEnabled());
}

QString DesktopShellViewModel::piperFileOutputExecutionStatus() const {
    return controller_.piperFileOutputExecutionStatus();
}

QString DesktopShellViewModel::piperFileOutputExecutionSummary() const {
    return controller_.piperFileOutputExecutionSummary();
}

QString DesktopShellViewModel::piperFileOutputAudioPathSummary() const {
    return controller_.piperFileOutputAudioPathSummary();
}

QString DesktopShellViewModel::whisperPreparationReadinessStatus() const {
    return controller_.whisperPreparationReadinessStatus();
}

QString DesktopShellViewModel::whisperPreparationReadinessSummary() const {
    return controller_.whisperPreparationReadinessSummary();
}

QString DesktopShellViewModel::voiceRuntimeReadinessSummary() const {
    return controller_.voiceRuntimeReadinessSummary();
}

QString DesktopShellViewModel::voiceRuntimeHealth() const {
    return controller_.voiceRuntimeHealth();
}

int DesktopShellViewModel::voiceRuntimeConfiguredCount() const {
    return controller_.voiceRuntimeConfiguredCount();
}

int DesktopShellViewModel::voiceRuntimeMissingCount() const {
    return controller_.voiceRuntimeMissingCount();
}

int DesktopShellViewModel::voiceRuntimeRefusedCount() const {
    return controller_.voiceRuntimeRefusedCount();
}

QString DesktopShellViewModel::voiceRuntimePermissionFoundationSummary() const {
    return controller_.voiceRuntimePermissionFoundationSummary();
}

QString DesktopShellViewModel::voiceRuntimeSandboxSummary() const {
    return controller_.voiceRuntimeSandboxSummary();
}

QString DesktopShellViewModel::voiceRuntimeSafetyReportSummary() const {
    return controller_.voiceRuntimeSafetyReportSummary();
}

QStringList DesktopShellViewModel::voiceRuntimeReadinessChecks() const {
    return controller_.voiceRuntimeReadinessChecks();
}

QString DesktopShellViewModel::whisperRuntimeStatus() const {
    return controller_.whisperRuntimeStatus();
}

QString DesktopShellViewModel::whisperRuntimeReadinessSummary() const {
    return controller_.whisperRuntimeReadinessSummary();
}

QString DesktopShellViewModel::whisperRuntimePathSummary() const {
    return controller_.whisperRuntimePathSummary();
}

QString DesktopShellViewModel::whisperTranscriptionStatus() const {
    return controller_.whisperTranscriptionStatus();
}

QString DesktopShellViewModel::whisperTranscriptionReadinessSummary() const {
    return controller_.whisperTranscriptionReadinessSummary();
}

QString DesktopShellViewModel::whisperTranscriptionLastSummary() const {
    return controller_.whisperTranscriptionLastSummary();
}

QString DesktopShellViewModel::whisperTranscriptionFallbackSummary() const {
    return controller_.whisperTranscriptionFallbackSummary();
}

QString DesktopShellViewModel::whisperTranscriptionSafetySummary() const {
    return controller_.whisperTranscriptionSafetySummary();
}

QStringList DesktopShellViewModel::whisperTranscriptionTraceSummaries() const {
    return controller_.whisperTranscriptionTraceSummaries();
}

QString DesktopShellViewModel::piperRuntimeStatus() const {
    return controller_.piperRuntimeStatus();
}

QString DesktopShellViewModel::piperRuntimeReadinessSummary() const {
    return controller_.piperRuntimeReadinessSummary();
}

QString DesktopShellViewModel::piperRuntimePathSummary() const {
    return controller_.piperRuntimePathSummary();
}

// QML-facing convenience API keeps explicit path arguments to avoid a larger invokable contract
// change. NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void DesktopShellViewModel::applyVoiceConfigurationPaths(const QString& piperBinaryPath,
                                                         const QString& piperModelPath,
                                                         const QString& whisperBinaryPath,
                                                         const QString& whisperModelPath) {
    setPiperBinaryPath(piperBinaryPath);
    setPiperModelPath(piperModelPath);
    setWhisperBinaryPath(whisperBinaryPath);
    setWhisperModelPath(whisperModelPath);
}

QString DesktopShellViewModel::autoDetectVoicePaths() {
    const QString result = settings_.autoDetectVoicePaths();
    controller_.setPiperBinaryPath(settings_.piperBinaryPath());
    controller_.setPiperModelPath(settings_.piperModelPath());
    controller_.setWhisperBinaryPath(settings_.whisperBinaryPath());
    controller_.setWhisperModelPath(settings_.whisperModelPath());
    emit voiceConfigurationChanged();
    return result;
}

QVariantMap DesktopShellViewModel::autoDetectVoicePathStatus() {
    const QString summary = autoDetectVoicePaths();

    QVariantMap paths;
    auto addConfiguredPath = [&paths](const QString& key, const QString& current) {
        if (!current.isEmpty()) {
            paths.insert(key, current);
        }
    };
    addConfiguredPath(QStringLiteral("piperBinary"), settings_.piperBinaryPath());
    addConfiguredPath(QStringLiteral("piperModel"), settings_.piperModelPath());
    addConfiguredPath(QStringLiteral("kokoroModel"), settings_.kokoroModelPath());
    addConfiguredPath(QStringLiteral("whisperBinary"), settings_.whisperBinaryPath());
    addConfiguredPath(QStringLiteral("whisperModel"), settings_.whisperModelPath());

    QVariantMap result;
    result.insert(QStringLiteral("summary"), summary);
    result.insert(QStringLiteral("detected"),
                  !summary.startsWith(QStringLiteral("No local voice binaries")));
    result.insert(QStringLiteral("paths"), paths);
    return result;
}

QString DesktopShellViewModel::voiceStatusMessage(const QString& status) const {
    if (status.isEmpty()) return {};
    const auto code = status.section(QLatin1Char(':'), 0, 0).trimmed();
    if (code == "ModelUnavailable" || code == "RuntimeUnavailable")
        return tr("Voice input needs a speech engine and model. Configure Local Whisper or choose system dictation in Voice & Audio.");
    if (code == "MicrophonePermissionDenied" || code == "PermissionDenied")
        return tr("Microphone or speech recognition access was denied. Allow access in your system privacy settings and try again.");
    if (code == "AudioDeviceUnavailable" || code == "CaptureFailure")
        return tr("The microphone is unavailable. Check your input device in Voice & Audio and try again.");
    if (code == "NoSpeechDetected") return tr("No speech was detected. Check your microphone and try again.");
    if (code == "TranscriptionFailure" || code == "ProviderFailure" || code == "Timeout")
        return tr("Speech recognition failed. Check your speech engine in Voice & Audio and try again.");
    if (code == "Cancelled") return tr("Voice input cancelled.");
    if (code == "UnsupportedFormat") return tr("This audio format is not supported by the selected speech engine.");
    // Native dictation already supplies localized recovery guidance.
    return status;
}

QString DesktopShellViewModel::voiceInputSource() const { return settings_.voiceInputSource(); }

void DesktopShellViewModel::setVoiceInputSource(const QString& source) {
    if (source != "local" && source != "system") return;
    systemDictation_.cancel();
    if (controller_.remote()) controller_.remote()->voiceAction("cancel");
    settings_.setVoiceInputSource(source);
    voiceUsingSystemFallback_ = false;
    voiceInputStatus_.clear();
    emit voiceInputChanged();
}
void DesktopShellViewModel::deliverVoiceTranscript(const QString& transcript) {
    if (voiceTestOnly_) emit voiceTestTranscriptionCompleted(transcript);
    else emit voiceTranscriptionCompleted(transcript);
}
bool DesktopShellViewModel::localSttReady() const {
    if (auto* client = controller_.remote())
        return client->voiceState().value("available").toBool();
    if (auto* session = controller_.audioSession())
        return session->sttInfo().readiness == core::AudioRuntimeReadiness::Ready;
    return false;
}
void DesktopShellViewModel::startVoiceCapture(bool testOnly) {
    if (voiceRecordingActive_ || systemDictation_.busy()) return;
    if (auto* client = controller_.remote()) {
        if (client->voiceState().value("state").toString() == "Transcribing") return;
    } else if (auto* session = controller_.audioSession()) {
        if (session->state() == core::VoiceInteractionState::Transcribing) return;
    }
    voiceTestOnly_ = testOnly;
    voiceInputStatus_.clear();
    lastVoiceTranscript_.clear();
    voiceUsingSystemFallback_ = false;
    // Local Whisper needs a configured runtime and model. Native dictation
    // needs no setup, so prefer it over a dead microphone button. The user is
    // told which backend is actually capturing, not left guessing.
    const auto useNativeDictation = [this] {
        voiceUsingSystemFallback_ = true;
        voiceInputStatus_ =
            tr("Local Whisper is not ready, so system dictation is being used instead.");
        systemDictation_.start(settings_.appLanguage());
    };
    if (voiceInputSource() == "system") {
        systemDictation_.start(settings_.appLanguage());
    } else if (auto* client = controller_.remote()) {
        // The desktop app always reaches voice through the daemon. When it
        // refuses the start (Whisper model missing, runtime busy, daemon still
        // connecting) the mic button would otherwise stay dead, so fall back to
        // the platform's native dictation, which needs no setup.
        if (!client->voiceAction("start")) {
            if (systemDictation_.supported()) useNativeDictation();
            else voiceInputStatus_ = tr("Local voice input is unavailable. Configure Whisper and enable transcription in Settings → Voice & Audio, or choose system dictation.");
        }
    } else if (auto* session = controller_.audioSession()) {
        if (localSttReady()) session->startPushToTalk(core::VoiceInteractionMode::Dictation);
        else voiceInputStatus_ = tr("Local voice input is unavailable. Configure Whisper or choose system dictation.");
    } else {
        voiceInputStatus_ = tr("Local voice input is unavailable. Configure Whisper or choose system dictation.");
    }
    emit voiceInputChanged();
}
void DesktopShellViewModel::stopVoiceCapture() {
    if (voiceInputSource() == "system" || voiceUsingSystemFallback_) systemDictation_.stop();
    else if (auto* client = controller_.remote()) client->voiceAction("stop");
    else if (auto* session = controller_.audioSession()) session->stopPushToTalk();
}

void DesktopShellViewModel::transcribeAudioFile(const QString& path) {
    if (auto* session = controller_.audioSession())
        session->transcribeAudioFile(path);
}

bool DesktopShellViewModel::generatePiperTtsFile(const QString& text) {
    return controller_.generatePiperTtsFile(text);
}

bool DesktopShellViewModel::localChatInferenceEnabled() const {
    return controller_.localChatInferenceEnabled();
}

void DesktopShellViewModel::setLocalChatInferenceEnabled(bool enabled) {
    settings_.setLocalChatInferenceEnabled(enabled);
    if (controller_.localChatInferenceEnabled() != settings_.localChatInferenceEnabled()) {
        controller_.setLocalChatInferenceEnabled(settings_.localChatInferenceEnabled());
    }
}

QString DesktopShellViewModel::localChatInferenceStatus() const {
    return controller_.localChatInferenceStatus();
}

QString DesktopShellViewModel::localChatInferenceSummary() const {
    return controller_.localChatInferenceSummary();
}

bool DesktopShellViewModel::localChatSendAvailable() const {
    return controller_.localChatSendAvailable();
}

QString DesktopShellViewModel::localChatSendAvailabilitySummary() const {
    return controller_.localChatSendAvailabilitySummary();
}

QString DesktopShellViewModel::chatSendLifecycleState() const {
    return controller_.chatSendLifecycleState();
}

QString DesktopShellViewModel::chatSendLifecycleSummary() const {
    return controller_.chatSendLifecycleSummary();
}

bool DesktopShellViewModel::chatGenerationActive() const {
    return controller_.chatGenerationActive();
}

QString DesktopShellViewModel::activeChatProviderId() const {
    return controller_.activeChatProviderId();
}

QString DesktopShellViewModel::activeChatModelId() const {
    return controller_.activeChatModelId();
}

QString DesktopShellViewModel::chatErrorCategory() const {
    return controller_.chatErrorCategory();
}

QString DesktopShellViewModel::chatCatalogState() const {
    return controller_.chatCatalogState();
}

QString DesktopShellViewModel::chatProviderKind() const {
    return controller_.chatProviderKind();
}

QVariantMap DesktopShellViewModel::chatInputCapabilities() const {
    return controller_.chatInputCapabilities();
}

bool DesktopShellViewModel::promptContextInjectionEnabled() const {
    return controller_.promptContextInjectionEnabled();
}

void DesktopShellViewModel::setPromptContextInjectionEnabled(bool enabled) {
    settings_.setPromptContextInjectionEnabled(enabled);
    if (controller_.promptContextInjectionEnabled() != settings_.promptContextInjectionEnabled()) {
        controller_.setPromptContextInjectionEnabled(settings_.promptContextInjectionEnabled());
    }
}

QString DesktopShellViewModel::promptContextInjectionStatus() const {
    return controller_.promptContextInjectionStatus();
}

QString DesktopShellViewModel::promptContextInjectionSummary() const {
    return controller_.promptContextInjectionSummary();
}

int DesktopShellViewModel::promptContextInjectedBlockCount() const {
    return controller_.promptContextInjectedBlockCount();
}

QString DesktopShellViewModel::promptContextSourceSummary() const {
    return controller_.promptContextSourceSummary();
}

QString DesktopShellViewModel::promptContextSizeSummary() const {
    return controller_.promptContextSizeSummary();
}

QString DesktopShellViewModel::promptContextUsedSummary() const {
    return controller_.promptContextUsedSummary();
}

int DesktopShellViewModel::promptContextUsedMemoryCount() const {
    return controller_.promptContextUsedMemoryCount();
}

QString DesktopShellViewModel::contextBudgetUsageSummary() const {
    return controller_.contextBudgetUsageSummary();
}

int DesktopShellViewModel::contextIncludedCandidateCount() const {
    return controller_.contextIncludedCandidateCount();
}

int DesktopShellViewModel::contextExcludedCandidateCount() const {
    return controller_.contextExcludedCandidateCount();
}

QStringList DesktopShellViewModel::contextAssemblyTraceSummaries() const {
    return controller_.contextAssemblyTraceSummaries();
}

QStringList DesktopShellViewModel::promptContextBlockSummaries() const {
    return controller_.promptContextBlockSummaries();
}

QString DesktopShellViewModel::conversationWindowStatus() const {
    return controller_.conversationWindowStatus();
}

QString DesktopShellViewModel::conversationWindowSummary() const {
    return controller_.conversationWindowSummary();
}

QString DesktopShellViewModel::conversationWindowBudgetSummary() const {
    return controller_.conversationWindowBudgetSummary();
}

int DesktopShellViewModel::conversationWindowBudgetCharacters() const {
    return controller_.conversationWindowBudgetCharacters();
}

int DesktopShellViewModel::conversationWindowIncludedMessageCount() const {
    return controller_.conversationWindowIncludedMessageCount();
}

int DesktopShellViewModel::conversationWindowTruncatedMessageCount() const {
    return controller_.conversationWindowTruncatedMessageCount();
}

int DesktopShellViewModel::conversationWindowOmittedMessageCount() const {
    return controller_.conversationWindowOmittedMessageCount();
}

QString DesktopShellViewModel::conversationSummaryStatus() const {
    return controller_.conversationSummaryStatus();
}

QString DesktopShellViewModel::conversationSummaryText() const {
    return controller_.conversationSummaryText();
}

QString DesktopShellViewModel::conversationSummaryBudgetSummary() const {
    return controller_.conversationSummaryBudgetSummary();
}

int DesktopShellViewModel::conversationSummaryBudgetCharacters() const {
    return controller_.conversationSummaryBudgetCharacters();
}

int DesktopShellViewModel::conversationSummaryBlockCount() const {
    return controller_.conversationSummaryBlockCount();
}

int DesktopShellViewModel::conversationSummaryMessageCount() const {
    return controller_.conversationSummaryMessageCount();
}

int DesktopShellViewModel::conversationSummaryOmittedMessageCount() const {
    return controller_.conversationSummaryOmittedMessageCount();
}

int DesktopShellViewModel::conversationSummaryTruncatedBlockCount() const {
    return controller_.conversationSummaryTruncatedBlockCount();
}

QStringList DesktopShellViewModel::conversationSummaryBlockSummaries() const {
    return controller_.conversationSummaryBlockSummaries();
}

QString DesktopShellViewModel::retrievalPlanningStatus() const {
    return controller_.retrievalPlanningStatus();
}

QString DesktopShellViewModel::retrievalPlanningSummary() const {
    return controller_.retrievalPlanningSummary();
}

QString DesktopShellViewModel::retrievalPlanningReadiness() const {
    return controller_.retrievalPlanningReadiness();
}

QString DesktopShellViewModel::retrievalPlanningBudgetSummary() const {
    return controller_.retrievalPlanningBudgetSummary();
}

QString DesktopShellViewModel::retrievalPlanningSourceSummary() const {
    return controller_.retrievalPlanningSourceSummary();
}

int DesktopShellViewModel::retrievalPlanningSelectedSourceCount() const {
    return controller_.retrievalPlanningSelectedSourceCount();
}

int DesktopShellViewModel::retrievalPlanningExcludedSourceCount() const {
    return controller_.retrievalPlanningExcludedSourceCount();
}

int DesktopShellViewModel::retrievalPlanningSelectedCandidateCount() const {
    return controller_.retrievalPlanningSelectedCandidateCount();
}

int DesktopShellViewModel::retrievalPlanningExcludedCandidateCount() const {
    return controller_.retrievalPlanningExcludedCandidateCount();
}

int DesktopShellViewModel::retrievalPlanningTruncatedCandidateCount() const {
    return controller_.retrievalPlanningTruncatedCandidateCount();
}

QStringList DesktopShellViewModel::retrievalPlanningSourceSummaries() const {
    return controller_.retrievalPlanningSourceSummaries();
}

QString DesktopShellViewModel::memoryRelevanceSummaryText() const {
    return controller_.memoryRelevanceSummaryText();
}

QString DesktopShellViewModel::memoryRelevanceBudgetSummary() const {
    return controller_.memoryRelevanceBudgetSummary();
}

int DesktopShellViewModel::memoryRelevanceIncludedCount() const {
    return controller_.memoryRelevanceIncludedCount();
}

int DesktopShellViewModel::memoryRelevanceExcludedCount() const {
    return controller_.memoryRelevanceExcludedCount();
}

QStringList DesktopShellViewModel::memoryRelevanceTraceSummaries() const {
    return controller_.memoryRelevanceTraceSummaries();
}

QStringList DesktopShellViewModel::memoryRelevanceExclusionSummaries() const {
    return controller_.memoryRelevanceExclusionSummaries();
}

QString DesktopShellViewModel::conversationSalienceSummaryText() const {
    return controller_.conversationSalienceSummaryText();
}

QString DesktopShellViewModel::conversationSalienceBudgetSummary() const {
    return controller_.conversationSalienceBudgetSummary();
}

QString DesktopShellViewModel::conversationSalienceAllocationSummary() const {
    return controller_.conversationSalienceAllocationSummary();
}

int DesktopShellViewModel::conversationSalienceIncludedCount() const {
    return controller_.conversationSalienceIncludedCount();
}

int DesktopShellViewModel::conversationSalienceExcludedCount() const {
    return controller_.conversationSalienceExcludedCount();
}

int DesktopShellViewModel::conversationSalienceTruncatedCount() const {
    return controller_.conversationSalienceTruncatedCount();
}

QStringList DesktopShellViewModel::conversationSalienceTraceSummaries() const {
    return controller_.conversationSalienceTraceSummaries();
}

QStringList DesktopShellViewModel::conversationSalienceExclusionSummaries() const {
    return controller_.conversationSalienceExclusionSummaries();
}

QString DesktopShellViewModel::conversationCompressionStatus() const {
    return controller_.conversationCompressionStatus();
}

QString DesktopShellViewModel::conversationCompressionReadinessSummary() const {
    return controller_.conversationCompressionReadinessSummary();
}

QString DesktopShellViewModel::conversationCompressionPressureSummary() const {
    return controller_.conversationCompressionPressureSummary();
}

int DesktopShellViewModel::conversationCompressionCandidateCount() const {
    return controller_.conversationCompressionCandidateCount();
}

int DesktopShellViewModel::conversationCompressionSelectedCandidateCount() const {
    return controller_.conversationCompressionSelectedCandidateCount();
}

QString DesktopShellViewModel::conversationCompressionFallbackReason() const {
    return controller_.conversationCompressionFallbackReason();
}

QString DesktopShellViewModel::conversationCompressionTraceSummary() const {
    return controller_.conversationCompressionTraceSummary();
}

QString DesktopShellViewModel::conversationCompressionBudgetSummary() const {
    return controller_.conversationCompressionBudgetSummary();
}

QStringList DesktopShellViewModel::conversationCompressionCandidateSummaries() const {
    return controller_.conversationCompressionCandidateSummaries();
}

QStringList DesktopShellViewModel::conversationCompressionTraceSummaries() const {
    return controller_.conversationCompressionTraceSummaries();
}

bool DesktopShellViewModel::conversationSummaryAvailable() const {
    return controller_.conversationSummaryAvailable();
}

QString DesktopShellViewModel::conversationSummaryGenerationStatus() const {
    return controller_.conversationSummaryGenerationStatus();
}

QString DesktopShellViewModel::conversationSummaryReadinessSummary() const {
    return controller_.conversationSummaryReadinessSummary();
}

QString DesktopShellViewModel::conversationSummaryBlockedReason() const {
    return controller_.conversationSummaryBlockedReason();
}

QString DesktopShellViewModel::conversationSummaryEstimatedCompressionGain() const {
    return controller_.conversationSummaryEstimatedCompressionGain();
}

QString DesktopShellViewModel::conversationSummaryPreviewSummary() const {
    return controller_.conversationSummaryPreviewSummary();
}

QString DesktopShellViewModel::conversationSummaryPersistenceSummary() const {
    return controller_.conversationSummaryPersistenceSummary();
}

QString DesktopShellViewModel::conversationSummaryInjectionSummary() const {
    return controller_.conversationSummaryInjectionSummary();
}

QString DesktopShellViewModel::summaryContinuityStatus() const {
    return controller_.summaryContinuityStatus();
}

QString DesktopShellViewModel::summaryContinuityFreshnessSummary() const {
    return controller_.summaryContinuityFreshnessSummary();
}

QString DesktopShellViewModel::summaryContinuityCoverageSummary() const {
    return controller_.summaryContinuityCoverageSummary();
}

QString DesktopShellViewModel::summaryContinuityContributionSummary() const {
    return controller_.summaryContinuityContributionSummary();
}

QString DesktopShellViewModel::summaryContinuityFallbackSummary() const {
    return controller_.summaryContinuityFallbackSummary();
}

QString DesktopShellViewModel::summaryContinuityOrderingSummary() const {
    return controller_.summaryContinuityOrderingSummary();
}

QString DesktopShellViewModel::summaryContinuityBudgetTrace() const {
    return controller_.summaryContinuityBudgetTrace();
}

bool DesktopShellViewModel::contextExplainabilityEnabled() const {
    return settings_.contextExplainabilityVisible();
}

bool DesktopShellViewModel::contextExplainabilityVisible() const {
    return settings_.contextExplainabilityVisible();
}

void DesktopShellViewModel::setContextExplainabilityVisible(bool visible) {
    settings_.setContextExplainabilityVisible(visible);
}

QString DesktopShellViewModel::contextReasoningSummary() const {
    return controller_.contextReasoningSummary();
}

QString DesktopShellViewModel::contextReasoningBudgetSummary() const {
    return controller_.contextReasoningBudgetSummary();
}

QString DesktopShellViewModel::contextReasoningOrderingSummary() const {
    return controller_.contextReasoningOrderingSummary();
}

QString DesktopShellViewModel::contextReasoningFallbackSummary() const {
    return controller_.contextReasoningFallbackSummary();
}

QStringList DesktopShellViewModel::contextReasoningContributionSummaries() const {
    return controller_.contextReasoningContributionSummaries();
}

QStringList DesktopShellViewModel::contextReasoningInclusionHints() const {
    return controller_.contextReasoningInclusionHints();
}

QStringList DesktopShellViewModel::contextReasoningExclusionHints() const {
    return controller_.contextReasoningExclusionHints();
}

QStringList DesktopShellViewModel::contextReasoningDeveloperTraces() const {
    return controller_.contextReasoningDeveloperTraces();
}

QStringList DesktopShellViewModel::conversationSummaryCandidateSegments() const {
    return controller_.conversationSummaryCandidateSegments();
}

QStringList DesktopShellViewModel::conversationSummaryGenerationTraceSummaries() const {
    return controller_.conversationSummaryGenerationTraceSummaries();
}

core::SemanticRetrievalPolicy DesktopShellViewModel::semanticRetrievalPolicy() const {
    return controller_.semanticRetrievalPolicy();
}

bool DesktopShellViewModel::semanticRetrievalEnabled() const {
    return controller_.semanticRetrievalEnabled();
}

QString DesktopShellViewModel::semanticRetrievalStatus() const {
    return controller_.semanticRetrievalStatus();
}

QString DesktopShellViewModel::semanticRetrievalSummary() const {
    return controller_.semanticRetrievalSummary();
}

QString DesktopShellViewModel::semanticReadiness() const {
    return controller_.semanticReadiness();
}

QString DesktopShellViewModel::embeddingProviderReadiness() const {
    return controller_.embeddingProviderReadiness();
}

QString DesktopShellViewModel::embeddingProviderSummary() const {
    return controller_.embeddingProviderSummary();
}

QString DesktopShellViewModel::vectorIndexReadiness() const {
    return controller_.vectorIndexReadiness();
}

QString DesktopShellViewModel::vectorIndexSummary() const {
    return controller_.vectorIndexSummary();
}

int DesktopShellViewModel::vectorIndexedItemCount() const {
    return controller_.vectorIndexedItemCount();
}

QString DesktopShellViewModel::semanticProviderMode() const {
    return controller_.semanticProviderMode();
}

QString DesktopShellViewModel::selectedSemanticProviderName() const {
    return controller_.selectedSemanticProviderName();
}

QString DesktopShellViewModel::semanticProviderReadiness() const {
    return controller_.semanticProviderReadiness();
}

QString DesktopShellViewModel::semanticProviderHealth() const {
    return controller_.semanticProviderHealth();
}

QString DesktopShellViewModel::semanticProviderStatusSummary() const {
    return controller_.semanticProviderStatusSummary();
}

QString DesktopShellViewModel::semanticActivationReadiness() const {
    return controller_.semanticActivationReadiness();
}

QString DesktopShellViewModel::semanticActivationSummary() const {
    return controller_.semanticActivationSummary();
}

QStringList DesktopShellViewModel::semanticProviderCapabilitySummaries() const {
    return controller_.semanticProviderCapabilitySummaries();
}

QStringList DesktopShellViewModel::semanticActivationRequiredSteps() const {
    return controller_.semanticActivationRequiredSteps();
}

QStringList DesktopShellViewModel::semanticRetrievalReadinessChecks() const {
    return controller_.semanticRetrievalReadinessChecks();
}

QString DesktopShellViewModel::semanticCandidateStatus() const {
    return controller_.semanticCandidateStatus();
}

QString DesktopShellViewModel::semanticCandidateSummary() const {
    return controller_.semanticCandidateSummary();
}

QString DesktopShellViewModel::semanticCandidateBudgetSummary() const {
    return controller_.semanticCandidateBudgetSummary();
}

QString DesktopShellViewModel::semanticCandidateArbitrationSummary() const {
    return controller_.semanticCandidateArbitrationSummary();
}

int DesktopShellViewModel::semanticCandidateCount() const {
    return controller_.semanticCandidateCount();
}

int DesktopShellViewModel::semanticCandidateSelectedCount() const {
    return controller_.semanticCandidateSelectedCount();
}

int DesktopShellViewModel::semanticCandidateExcludedCount() const {
    return controller_.semanticCandidateExcludedCount();
}

int DesktopShellViewModel::semanticCandidateTruncatedCount() const {
    return controller_.semanticCandidateTruncatedCount();
}

QStringList DesktopShellViewModel::semanticCandidateParticipationSummaries() const {
    return controller_.semanticCandidateParticipationSummaries();
}

QString DesktopShellViewModel::hybridRetrievalStatus() const {
    return controller_.hybridRetrievalStatus();
}

QString DesktopShellViewModel::hybridRetrievalReadiness() const {
    return controller_.hybridRetrievalReadiness();
}

QString DesktopShellViewModel::hybridRetrievalSummary() const {
    return controller_.hybridRetrievalSummary();
}

QStringList DesktopShellViewModel::hybridRetrievalReadinessChecks() const {
    return controller_.hybridRetrievalReadinessChecks();
}

QString DesktopShellViewModel::semanticArbitrationStatus() const {
    return controller_.semanticArbitrationStatus();
}

QString DesktopShellViewModel::semanticArbitrationReadiness() const {
    return controller_.semanticArbitrationReadiness();
}

QString DesktopShellViewModel::semanticArbitrationSummary() const {
    return controller_.semanticArbitrationSummary();
}

QString DesktopShellViewModel::semanticArbitrationBudgetSummary() const {
    return controller_.semanticArbitrationBudgetSummary();
}

QStringList DesktopShellViewModel::semanticArbitrationSelectionSummaries() const {
    return controller_.semanticArbitrationSelectionSummaries();
}

QStringList DesktopShellViewModel::semanticArbitrationChecks() const {
    return controller_.semanticArbitrationChecks();
}

QString DesktopShellViewModel::embeddingRuntimeReadiness() const {
    return controller_.embeddingRuntimeReadiness();
}

QString DesktopShellViewModel::embeddingRuntimeSummary() const {
    return controller_.embeddingRuntimeSummary();
}

QString DesktopShellViewModel::embeddingRuntimeBudgetSummary() const {
    return controller_.embeddingRuntimeBudgetSummary();
}

QStringList DesktopShellViewModel::embeddingRuntimeRequirementSummaries() const {
    return controller_.embeddingRuntimeRequirementSummaries();
}

QStringList DesktopShellViewModel::embeddingRuntimeConstraintSummaries() const {
    return controller_.embeddingRuntimeConstraintSummaries();
}

QString DesktopShellViewModel::isolatedEmbeddingRuntimeStatus() const {
    return controller_.isolatedEmbeddingRuntimeStatus();
}

QString DesktopShellViewModel::isolatedEmbeddingRuntimeHealth() const {
    return controller_.isolatedEmbeddingRuntimeHealth();
}

QString DesktopShellViewModel::isolatedEmbeddingRuntimeReadiness() const {
    return controller_.isolatedEmbeddingRuntimeReadiness();
}

QString DesktopShellViewModel::isolatedEmbeddingRuntimeSummary() const {
    return controller_.isolatedEmbeddingRuntimeSummary();
}

QString DesktopShellViewModel::isolatedEmbeddingRuntimeBoundedState() const {
    return controller_.isolatedEmbeddingRuntimeBoundedState();
}

QStringList DesktopShellViewModel::isolatedEmbeddingRuntimeChecks() const {
    return controller_.isolatedEmbeddingRuntimeChecks();
}

QString DesktopShellViewModel::vectorPersistenceStatus() const {
    return controller_.vectorPersistenceStatus();
}

QString DesktopShellViewModel::vectorPersistenceHealth() const {
    return controller_.vectorPersistenceHealth();
}

QString DesktopShellViewModel::vectorPersistenceReadiness() const {
    return controller_.vectorPersistenceReadiness();
}

QString DesktopShellViewModel::vectorPersistenceSummary() const {
    return controller_.vectorPersistenceSummary();
}

QString DesktopShellViewModel::vectorPersistenceBoundedState() const {
    return controller_.vectorPersistenceBoundedState();
}

int DesktopShellViewModel::vectorPersistenceIndexedItemCount() const {
    return controller_.vectorPersistenceIndexedItemCount();
}

QStringList DesktopShellViewModel::vectorPersistenceChecks() const {
    return controller_.vectorPersistenceChecks();
}

QString DesktopShellViewModel::semanticSearchStatus() const {
    return controller_.semanticSearchStatus();
}

QString DesktopShellViewModel::semanticSearchReadiness() const {
    return controller_.semanticSearchReadiness();
}

QString DesktopShellViewModel::semanticSearchSummary() const {
    return controller_.semanticSearchSummary();
}

QString DesktopShellViewModel::semanticSearchBudgetSummary() const {
    return controller_.semanticSearchBudgetSummary();
}

QString DesktopShellViewModel::semanticSearchRuntimeState() const {
    return controller_.semanticSearchRuntimeState();
}

int DesktopShellViewModel::semanticSearchCandidateCount() const {
    return controller_.semanticSearchCandidateCount();
}

QString DesktopShellViewModel::semanticSearchArbitrationSummary() const {
    return controller_.semanticSearchArbitrationSummary();
}

QStringList DesktopShellViewModel::semanticSearchCandidateSummaries() const {
    return controller_.semanticSearchCandidateSummaries();
}

QStringList DesktopShellViewModel::semanticSearchChecks() const {
    return controller_.semanticSearchChecks();
}

QString DesktopShellViewModel::hybridBridgeStatus() const {
    return controller_.hybridBridgeStatus();
}

QString DesktopShellViewModel::hybridBridgeReadiness() const {
    return controller_.hybridBridgeReadiness();
}

QString DesktopShellViewModel::hybridBridgeSummary() const {
    return controller_.hybridBridgeSummary();
}

QString DesktopShellViewModel::hybridBridgeBudgetSummary() const {
    return controller_.hybridBridgeBudgetSummary();
}

QString DesktopShellViewModel::hybridBridgeSourceSummary() const {
    return controller_.hybridBridgeSourceSummary();
}

QString DesktopShellViewModel::hybridBridgeArbitrationSummary() const {
    return controller_.hybridBridgeArbitrationSummary();
}

QString DesktopShellViewModel::hybridBridgeFallbackSummary() const {
    return controller_.hybridBridgeFallbackSummary();
}

int DesktopShellViewModel::hybridBridgeCandidateCount() const {
    return controller_.hybridBridgeCandidateCount();
}

int DesktopShellViewModel::hybridBridgeSemanticFillCount() const {
    return controller_.hybridBridgeSemanticFillCount();
}

QStringList DesktopShellViewModel::hybridBridgeCandidateSummaries() const {
    return controller_.hybridBridgeCandidateSummaries();
}

QStringList DesktopShellViewModel::hybridBridgeChecks() const {
    return controller_.hybridBridgeChecks();
}

QString DesktopShellViewModel::semanticAcceptanceStatus() const {
    return controller_.semanticAcceptanceStatus();
}

QString DesktopShellViewModel::semanticAcceptanceReadiness() const {
    return controller_.semanticAcceptanceReadiness();
}

QString DesktopShellViewModel::semanticAcceptanceSummary() const {
    return controller_.semanticAcceptanceSummary();
}

QString DesktopShellViewModel::semanticAcceptanceBudgetSummary() const {
    return controller_.semanticAcceptanceBudgetSummary();
}

QString DesktopShellViewModel::semanticAcceptanceSourceSummary() const {
    return controller_.semanticAcceptanceSourceSummary();
}

QString DesktopShellViewModel::semanticAcceptanceArbitrationSummary() const {
    return controller_.semanticAcceptanceArbitrationSummary();
}

QString DesktopShellViewModel::semanticAcceptanceFallbackSummary() const {
    return controller_.semanticAcceptanceFallbackSummary();
}

int DesktopShellViewModel::semanticAcceptanceAcceptedCount() const {
    return controller_.semanticAcceptanceAcceptedCount();
}

int DesktopShellViewModel::semanticAcceptanceBudgetCharacters() const {
    return controller_.semanticAcceptanceBudgetCharacters();
}

QStringList DesktopShellViewModel::semanticAcceptanceCandidateSummaries() const {
    return controller_.semanticAcceptanceCandidateSummaries();
}

QStringList DesktopShellViewModel::semanticAcceptanceChecks() const {
    return controller_.semanticAcceptanceChecks();
}

QString DesktopShellViewModel::semanticSupplementAssemblyStatus() const {
    return controller_.semanticSupplementAssemblyStatus();
}

QString DesktopShellViewModel::semanticSupplementAssemblyReadiness() const {
    return controller_.semanticSupplementAssemblyReadiness();
}

QString DesktopShellViewModel::semanticSupplementAssemblySummary() const {
    return controller_.semanticSupplementAssemblySummary();
}

QString DesktopShellViewModel::semanticSupplementAssemblyBudgetSummary() const {
    return controller_.semanticSupplementAssemblyBudgetSummary();
}

QString DesktopShellViewModel::semanticSupplementAssemblySafetySummary() const {
    return controller_.semanticSupplementAssemblySafetySummary();
}

int DesktopShellViewModel::semanticSupplementAssemblyBlockCount() const {
    return controller_.semanticSupplementAssemblyBlockCount();
}

int DesktopShellViewModel::semanticSupplementAssemblyBudgetCharacters() const {
    return controller_.semanticSupplementAssemblyBudgetCharacters();
}

QStringList DesktopShellViewModel::semanticSupplementAssemblyChecks() const {
    return controller_.semanticSupplementAssemblyChecks();
}

QString DesktopShellViewModel::semanticPromptAuthorityStatus() const {
    return controller_.semanticPromptAuthorityStatus();
}

QString DesktopShellViewModel::semanticPromptAuthorityDecisionSummary() const {
    return controller_.semanticPromptAuthorityDecisionSummary();
}

QString DesktopShellViewModel::semanticPromptAuthoritySafetySummary() const {
    return controller_.semanticPromptAuthoritySafetySummary();
}

QString DesktopShellViewModel::semanticPromptAuthorityReadinessSummary() const {
    return controller_.semanticPromptAuthorityReadinessSummary();
}

QString DesktopShellViewModel::semanticPromptAuthorityFallbackSummary() const {
    return controller_.semanticPromptAuthorityFallbackSummary();
}

QString DesktopShellViewModel::semanticPromptAuthorityAuditSummary() const {
    return controller_.semanticPromptAuthorityAuditSummary();
}

int DesktopShellViewModel::semanticPromptAuthorityWouldIncludeBlockCount() const {
    return controller_.semanticPromptAuthorityWouldIncludeBlockCount();
}

QStringList DesktopShellViewModel::semanticPromptAuthorityChecks() const {
    return controller_.semanticPromptAuthorityChecks();
}

bool DesktopShellViewModel::semanticPromptInclusionEnabled() const {
    return controller_.semanticPromptInclusionEnabled();
}

void DesktopShellViewModel::setSemanticPromptInclusionEnabled(bool enabled) {
    settings_.setSemanticPromptInclusionEnabled(enabled);
    if (controller_.semanticPromptInclusionEnabled() !=
        settings_.semanticPromptInclusionEnabled()) {
        controller_.setSemanticPromptInclusionEnabled(settings_.semanticPromptInclusionEnabled());
    }
}

QString DesktopShellViewModel::semanticPromptInclusionStatus() const {
    return controller_.semanticPromptInclusionStatus();
}

QString DesktopShellViewModel::semanticPromptInclusionSummary() const {
    return controller_.semanticPromptInclusionSummary();
}

int DesktopShellViewModel::semanticPromptInclusionIncludedCount() const {
    return controller_.semanticPromptInclusionIncludedCount();
}

QString DesktopShellViewModel::semanticPromptInclusionBudgetSummary() const {
    return controller_.semanticPromptInclusionBudgetSummary();
}

QString DesktopShellViewModel::semanticPromptInclusionFallbackSummary() const {
    return controller_.semanticPromptInclusionFallbackSummary();
}

QString DesktopShellViewModel::semanticPromptInclusionAuditSummary() const {
    return controller_.semanticPromptInclusionAuditSummary();
}

bool DesktopShellViewModel::semanticPromptInclusionDeterministicAuthorityPreserved() const {
    return controller_.semanticPromptInclusionDeterministicAuthorityPreserved();
}

QStringList DesktopShellViewModel::semanticPromptInclusionChecks() const {
    return controller_.semanticPromptInclusionChecks();
}

bool DesktopShellViewModel::localInferenceStreamingEnabled() const {
    return controller_.localInferenceStreamingEnabled();
}

void DesktopShellViewModel::setLocalInferenceStreamingEnabled(bool enabled) {
    settings_.setLocalInferenceStreamingEnabled(enabled);
    if (controller_.localInferenceStreamingEnabled() !=
        settings_.localInferenceStreamingEnabled()) {
        controller_.setLocalInferenceStreamingEnabled(settings_.localInferenceStreamingEnabled());
    }
}

int DesktopShellViewModel::localInferenceTimeoutMs() const {
    return controller_.localInferenceTimeoutMs();
}

void DesktopShellViewModel::setLocalInferenceTimeoutMs(int timeoutMs) {
    settings_.setLocalInferenceTimeoutMs(timeoutMs);
    if (controller_.localInferenceTimeoutMs() != settings_.localInferenceTimeoutMs()) {
        controller_.setLocalInferenceTimeoutMs(settings_.localInferenceTimeoutMs());
    }
}

double DesktopShellViewModel::localInferenceTemperature() const {
    return controller_.localInferenceTemperature();
}

void DesktopShellViewModel::setLocalInferenceTemperature(double temperature) {
    settings_.setLocalInferenceTemperature(temperature);
    if (controller_.localInferenceTemperature() != settings_.localInferenceTemperature()) {
        controller_.setLocalInferenceTemperature(settings_.localInferenceTemperature());
    }
}

double DesktopShellViewModel::localInferenceTopP() const {
    return controller_.localInferenceTopP();
}

void DesktopShellViewModel::setLocalInferenceTopP(double topP) {
    settings_.setLocalInferenceTopP(topP);
    if (controller_.localInferenceTopP() != settings_.localInferenceTopP()) {
        controller_.setLocalInferenceTopP(settings_.localInferenceTopP());
    }
}

int DesktopShellViewModel::localInferenceMaxTokens() const {
    return controller_.localInferenceMaxTokens();
}

void DesktopShellViewModel::setLocalInferenceMaxTokens(int maxTokens) {
    settings_.setLocalInferenceMaxTokens(maxTokens);
    if (controller_.localInferenceMaxTokens() != settings_.localInferenceMaxTokens()) {
        controller_.setLocalInferenceMaxTokens(settings_.localInferenceMaxTokens());
    }
}

bool DesktopShellViewModel::localInferenceBusy() const {
    return controller_.localInferenceBusy();
}

QString DesktopShellViewModel::localInferenceRuntimeState() const {
    return controller_.localInferenceRuntimeState();
}

QString DesktopShellViewModel::localInferenceStatus() const {
    return controller_.localInferenceStatus();
}

QString DesktopShellViewModel::localInferenceSummary() const {
    return controller_.localInferenceSummary();
}

QString DesktopShellViewModel::localInferenceLastResponseSummary() const {
    return controller_.localInferenceLastResponseSummary();
}

QString DesktopShellViewModel::localInferenceLatencySummary() const {
    return controller_.localInferenceLatencySummary();
}

QStringList DesktopShellViewModel::localInferenceTraceSummaries() const {
    return controller_.localInferenceTraceSummaries();
}

bool DesktopShellViewModel::localInferenceStreamingAvailable() const {
    return controller_.localInferenceStreamingAvailable();
}

QString DesktopShellViewModel::localInferenceStreamStatus() const {
    return controller_.localInferenceStreamStatus();
}

QString DesktopShellViewModel::localInferenceStreamSummary() const {
    return controller_.localInferenceStreamSummary();
}

QString DesktopShellViewModel::localInferenceStreamingText() const {
    return controller_.localInferenceStreamingText();
}

int DesktopShellViewModel::availableToolCount() const {
    return controller_.availableToolCount();
}

QStringList DesktopShellViewModel::availableToolIds() const {
    return controller_.availableToolIds();
}

QString DesktopShellViewModel::memoryStatus() const {
    return controller_.memoryStatus();
}

QString DesktopShellViewModel::chatHistoryStatus() const {
    return controller_.chatHistoryStatus();
}

QString DesktopShellViewModel::conversationStoreStatus() const {
    return controller_.conversationStoreStatus();
}

int DesktopShellViewModel::conversationStoreConversationCount() const {
    return controller_.conversationStoreConversationCount();
}

QString DesktopShellViewModel::activeConversationSummary() const {
    return controller_.activeConversationSummary();
}

QStringList DesktopShellViewModel::conversationStoreSummaries() const {
    return controller_.conversationStoreSummaries();
}

QString DesktopShellViewModel::activeConversationId() const {
    return controller_.activeConversationId();
}

bool DesktopShellViewModel::activeConversationArchived() const {
    return controller_.activeConversationArchived();
}

QString DesktopShellViewModel::activeConversationStateSummary() const {
    return controller_.activeConversationStateSummary();
}

QStringList DesktopShellViewModel::conversationIds() const {
    return controller_.conversationIds();
}

QStringList DesktopShellViewModel::conversationTitles() const {
    return controller_.conversationTitles();
}

QStringList DesktopShellViewModel::conversationActiveSummaries() const {
    return controller_.conversationActiveSummaries();
}

QStringList DesktopShellViewModel::conversationLastUpdatedSummaries() const {
    return controller_.conversationLastUpdatedSummaries();
}

QStringList DesktopShellViewModel::conversationMessageCountSummaries() const {
    return controller_.conversationMessageCountSummaries();
}

QStringList DesktopShellViewModel::conversationArchivedSummaries() const {
    return controller_.conversationArchivedSummaries();
}

QStringList DesktopShellViewModel::conversationPinnedSummaries() const {
    return controller_.conversationPinnedSummaries();
}

int DesktopShellViewModel::activeConversationCount() const {
    return controller_.activeConversationCount();
}

int DesktopShellViewModel::archivedConversationCount() const {
    return controller_.archivedConversationCount();
}

int DesktopShellViewModel::userCreatedConversationCount() const {
    return controller_.userCreatedConversationCount();
}

bool DesktopShellViewModel::conversationBrowserEmptyStateVisible() const {
    return controller_.conversationBrowserEmptyStateVisible();
}

QString DesktopShellViewModel::conversationBrowserEmptyStateSummary() const {
    return controller_.conversationBrowserEmptyStateSummary();
}

QString DesktopShellViewModel::conversationHistorySummaryText() const {
    return controller_.conversationHistorySummaryText();
}

QStringList DesktopShellViewModel::conversationHistorySummaryLines() const {
    return controller_.conversationHistorySummaryLines();
}

int DesktopShellViewModel::conversationHistoryMessageCount() const {
    return controller_.conversationHistoryMessageCount();
}

QString DesktopShellViewModel::conversationPersistenceStatus() const {
    return controller_.conversationPersistenceStatus();
}

QString DesktopShellViewModel::conversationLastSavedStatus() const {
    return controller_.conversationLastSavedStatus();
}

QString DesktopShellViewModel::conversationLastRestoredStatus() const {
    return controller_.conversationLastRestoredStatus();
}

QString DesktopShellViewModel::conversationBrowserStatus() const {
    return controller_.conversationBrowserStatus();
}

QString DesktopShellViewModel::conversationBrowserSummaryText() const {
    return controller_.conversationBrowserSummaryText();
}

int DesktopShellViewModel::conversationListEntryCount() const {
    return controller_.conversationListEntryCount();
}

QString DesktopShellViewModel::conversationListCurrentTitle() const {
    return controller_.conversationListCurrentTitle();
}

int DesktopShellViewModel::conversationListCurrentMessageCount() const {
    return controller_.conversationListCurrentMessageCount();
}

QString DesktopShellViewModel::conversationListCurrentPersistenceStatus() const {
    return controller_.conversationListCurrentPersistenceStatus();
}

QString DesktopShellViewModel::conversationListCurrentLastUpdatedSummary() const {
    return controller_.conversationListCurrentLastUpdatedSummary();
}

QString DesktopShellViewModel::conversationListCurrentSearchAvailabilitySummary() const {
    return controller_.conversationListCurrentSearchAvailabilitySummary();
}

QString DesktopShellViewModel::conversationListCurrentExportAvailabilitySummary() const {
    return controller_.conversationListCurrentExportAvailabilitySummary();
}

QString DesktopShellViewModel::conversationListCurrentSummary() const {
    return controller_.conversationListCurrentSummary();
}

QString DesktopShellViewModel::conversationCurrentStorageMode() const {
    return controller_.conversationCurrentStorageMode();
}

QString DesktopShellViewModel::conversationFutureStorageMode() const {
    return controller_.conversationFutureStorageMode();
}

QString DesktopShellViewModel::conversationMigrationReadiness() const {
    return controller_.conversationMigrationReadiness();
}

QString DesktopShellViewModel::conversationMigrationStatusSummary() const {
    return controller_.conversationMigrationStatusSummary();
}

QString DesktopShellViewModel::conversationSchemaStatusSummary() const {
    return controller_.conversationSchemaStatusSummary();
}

QString DesktopShellViewModel::conversationSearchQueryText() const {
    return controller_.conversationSearchQueryText();
}

QString DesktopShellViewModel::conversationSearchStatus() const {
    return controller_.conversationSearchStatus();
}

QString DesktopShellViewModel::conversationSearchSummaryText() const {
    return controller_.conversationSearchSummaryText();
}

int DesktopShellViewModel::conversationSearchResultCount() const {
    return controller_.conversationSearchResultCount();
}

QStringList DesktopShellViewModel::conversationSearchResultSummaries() const {
    return controller_.conversationSearchResultSummaries();
}

bool DesktopShellViewModel::conversationExportAvailable() const {
    return controller_.conversationExportAvailable();
}

QString DesktopShellViewModel::conversationExportReadinessStatus() const {
    return controller_.conversationExportReadinessStatus();
}

QString DesktopShellViewModel::conversationExportReadinessSummary() const {
    return controller_.conversationExportReadinessSummary();
}

QStringList DesktopShellViewModel::conversationExportReadinessChecks() const {
    return controller_.conversationExportReadinessChecks();
}

QString DesktopShellViewModel::conversationExportLastResultSummary() const {
    return controller_.conversationExportLastResultSummary();
}

QString DesktopShellViewModel::conversationExportLastStatus() const {
    return controller_.conversationExportLastStatus();
}

QString DesktopShellViewModel::conversationExportLastFileName() const {
    return controller_.conversationExportLastFileName();
}

int DesktopShellViewModel::conversationExportLastMessageCount() const {
    return controller_.conversationExportLastMessageCount();
}

QString DesktopShellViewModel::conversationExportLastTimestamp() const {
    return controller_.conversationExportLastTimestamp();
}

QString DesktopShellViewModel::conversationDuplicateLastStatus() const {
    return controller_.conversationDuplicateLastStatus();
}

QString DesktopShellViewModel::conversationDuplicateLastResultSummary() const {
    return controller_.conversationDuplicateLastResultSummary();
}

bool DesktopShellViewModel::conversationDeleteAvailable() const {
    return controller_.conversationDeleteAvailable();
}

QString DesktopShellViewModel::conversationDeletePolicyStatus() const {
    return controller_.conversationDeletePolicyStatus();
}

QString DesktopShellViewModel::conversationDeletePolicySummary() const {
    return controller_.conversationDeletePolicySummary();
}

QStringList DesktopShellViewModel::conversationDeletePolicyRequirements() const {
    return controller_.conversationDeletePolicyRequirements();
}

QString DesktopShellViewModel::conversationDeleteReadinessStatus() const {
    return controller_.conversationDeleteReadinessStatus();
}

QString DesktopShellViewModel::conversationDeleteReadinessSummary() const {
    return controller_.conversationDeleteReadinessSummary();
}

QStringList DesktopShellViewModel::conversationDeleteReadinessChecks() const {
    return controller_.conversationDeleteReadinessChecks();
}

QString DesktopShellViewModel::conversationDeleteLastStatus() const {
    return controller_.conversationDeleteLastStatus();
}

QString DesktopShellViewModel::conversationDeleteLastResultSummary() const {
    return controller_.conversationDeleteLastResultSummary();
}

int DesktopShellViewModel::memoryCandidateCount() const {
    return controller_.memoryCandidateCount();
}

int DesktopShellViewModel::pendingMemoryCandidateCount() const {
    return controller_.pendingMemoryCandidateCount();
}

int DesktopShellViewModel::approvedMemoryCandidateCount() const {
    return controller_.approvedMemoryCandidateCount();
}

int DesktopShellViewModel::rejectedMemoryCandidateCount() const {
    return controller_.rejectedMemoryCandidateCount();
}

int DesktopShellViewModel::archivedMemoryCandidateCount() const {
    return controller_.archivedMemoryCandidateCount();
}

int DesktopShellViewModel::committedMemoryCandidateCount() const {
    return controller_.committedMemoryCandidateCount();
}

QStringList DesktopShellViewModel::memoryCandidateIds() const {
    return controller_.memoryCandidateIds();
}

QStringList DesktopShellViewModel::memoryCandidateReviewStates() const {
    return controller_.memoryCandidateReviewStates();
}

QStringList DesktopShellViewModel::memoryCandidateCommitStatuses() const {
    return controller_.memoryCandidateCommitStatuses();
}

QStringList DesktopShellViewModel::memoryCandidateSummaries() const {
    return controller_.memoryCandidateSummaries();
}

QStringList DesktopShellViewModel::pendingMemoryCandidateSummaries() const {
    return controller_.pendingMemoryCandidateSummaries();
}

QStringList DesktopShellViewModel::approvedMemoryCandidateSummaries() const {
    return controller_.approvedMemoryCandidateSummaries();
}

QStringList DesktopShellViewModel::rejectedMemoryCandidateSummaries() const {
    return controller_.rejectedMemoryCandidateSummaries();
}

QStringList DesktopShellViewModel::archivedMemoryCandidateSummaries() const {
    return controller_.archivedMemoryCandidateSummaries();
}

QString DesktopShellViewModel::lastMemoryCandidateReviewStatus() const {
    return controller_.lastMemoryCandidateReviewStatus();
}

QString DesktopShellViewModel::lastMemoryCandidateReviewSummary() const {
    return controller_.lastMemoryCandidateReviewSummary();
}

QString DesktopShellViewModel::memoryCommitReadinessStatus() const {
    return controller_.memoryCommitReadinessStatus();
}

QString DesktopShellViewModel::memoryCommitReadinessSummary() const {
    return controller_.memoryCommitReadinessSummary();
}

QStringList DesktopShellViewModel::memoryCommitReadinessChecks() const {
    return controller_.memoryCommitReadinessChecks();
}

int DesktopShellViewModel::memoryCommitPlanCount() const {
    return controller_.memoryCommitPlanCount();
}

QString DesktopShellViewModel::memoryCommitTargetSummary() const {
    return controller_.memoryCommitTargetSummary();
}

QStringList DesktopShellViewModel::memoryCommitCandidateSummaries() const {
    return controller_.memoryCommitCandidateSummaries();
}

QString DesktopShellViewModel::lastMemoryCommitStatus() const {
    return controller_.lastMemoryCommitStatus();
}

QString DesktopShellViewModel::lastMemoryCommitResultSummary() const {
    return controller_.lastMemoryCommitResultSummary();
}

QString DesktopShellViewModel::memoryRecallPolicyStatus() const {
    return controller_.memoryRecallPolicyStatus();
}

QString DesktopShellViewModel::memoryRecallPolicySummary() const {
    return controller_.memoryRecallPolicySummary();
}

QString DesktopShellViewModel::memoryRecallQueryText() const {
    return controller_.memoryRecallQueryText();
}

QString DesktopShellViewModel::memoryRecallStatus() const {
    return controller_.memoryRecallStatus();
}

QString DesktopShellViewModel::memoryRecallSummaryText() const {
    return controller_.memoryRecallSummaryText();
}

int DesktopShellViewModel::memoryRecallResultCount() const {
    return controller_.memoryRecallResultCount();
}

QStringList DesktopShellViewModel::memoryRecallResultSummaries() const {
    return controller_.memoryRecallResultSummaries();
}

QString DesktopShellViewModel::contextAssemblyPolicyStatus() const {
    return controller_.contextAssemblyPolicyStatus();
}

QString DesktopShellViewModel::contextAssemblyPolicySummary() const {
    return controller_.contextAssemblyPolicySummary();
}

QString DesktopShellViewModel::contextAssemblyStatus() const {
    return controller_.contextAssemblyStatus();
}

QString DesktopShellViewModel::contextAssemblySummaryText() const {
    return controller_.contextAssemblySummaryText();
}

int DesktopShellViewModel::contextAssemblySourceCount() const {
    return controller_.contextAssemblySourceCount();
}

int DesktopShellViewModel::contextAssemblyAvailableSourceCount() const {
    return controller_.contextAssemblyAvailableSourceCount();
}

int DesktopShellViewModel::contextAssemblyCandidateBlockCount() const {
    return controller_.contextAssemblyCandidateBlockCount();
}

int DesktopShellViewModel::contextAssemblyEstimatedSize() const {
    return controller_.contextAssemblyEstimatedSize();
}

QString DesktopShellViewModel::conversationContextAvailability() const {
    return controller_.conversationContextAvailability();
}

QString DesktopShellViewModel::committedMemoryContextAvailability() const {
    return controller_.committedMemoryContextAvailability();
}

QString DesktopShellViewModel::runtimeMetadataContextAvailability() const {
    return controller_.runtimeMetadataContextAvailability();
}

QString DesktopShellViewModel::orchestrationContextAvailability() const {
    return controller_.orchestrationContextAvailability();
}

QStringList DesktopShellViewModel::contextAssemblySourceSummaries() const {
    return controller_.contextAssemblySourceSummaries();
}

QStringList DesktopShellViewModel::contextAssemblyReadinessChecks() const {
    return controller_.contextAssemblyReadinessChecks();
}

int DesktopShellViewModel::memoryEntryCount() const {
    return controller_.memoryEntryCount();
}

QString DesktopShellViewModel::memoryMaintenanceStatus() const {
    return controller_.memoryMaintenanceStatus();
}

QString DesktopShellViewModel::chatMaintenanceStatus() const {
    return controller_.chatMaintenanceStatus();
}

QString DesktopShellViewModel::currentModeName() const {
    return modeManager_.currentModeName();
}

void DesktopShellViewModel::setCurrentModeName(const QString& modeName) {
    modeManager_.setModeByName(modeName);
    settings_.setSelectedSystemMode(modeName);
}

QStringList DesktopShellViewModel::availableModes() const {
    return modeManager_.availableModes();
}

QString DesktopShellViewModel::selectedCloudProvider() const {
    return settings_.selectedCloudProvider();
}

void DesktopShellViewModel::setSelectedCloudProvider(const QString& provider) {
    settings_.setSelectedCloudProvider(provider);
    controller_.refreshOllamaStatus();
}

QString DesktopShellViewModel::webSearchProvider() const {
    return settings_.webSearchProvider();
}

void DesktopShellViewModel::setWebSearchProvider(const QString& provider) {
    settings_.setWebSearchProvider(provider);
}

QString DesktopShellViewModel::webSearchApiKey() const {
    return {};
}

void DesktopShellViewModel::setWebSearchApiKey(const QString& key) {
    if (key.trimmed().isEmpty())
        return;
    if (controller_.isRemote()) {
        setProductProviderCredential(QStringLiteral("web-search"), key);
        return;
    }
    settings_.setWebSearchApiKey(key);
}

int DesktopShellViewModel::webSearchMaxResults() const {
    return settings_.webSearchMaxResults();
}

void DesktopShellViewModel::setWebSearchMaxResults(int maxResults) {
    settings_.setWebSearchMaxResults(maxResults);
}

QString DesktopShellViewModel::semanticProvider() const {
    return settings_.semanticProvider();
}

void DesktopShellViewModel::setSemanticProvider(const QString& provider) {
    settings_.setSemanticProvider(provider);
    if (settings_.semanticProvider() == QStringLiteral("ollama")) {
        settings_.setPromptContextInjectionEnabled(true);
        settings_.setSemanticPromptInclusionEnabled(true);
    }
}

QString DesktopShellViewModel::semanticEmbeddingModel() const {
    return settings_.semanticEmbeddingModel();
}

void DesktopShellViewModel::setSemanticEmbeddingModel(const QString& model) {
    settings_.setSemanticEmbeddingModel(model);
}

QString DesktopShellViewModel::currentPage() const {
    return currentPage_;
}

void DesktopShellViewModel::setCurrentPage(const QString& page) {
    const auto normalized = normalizedPageOrDefault(page);
    if (normalized == currentPage_) {
        return;
    }

    currentPage_ = normalized;
    emit currentPageChanged();
}

QStringList DesktopShellViewModel::availablePages() const {
    return {
        QStringLiteral("Memory"),
        QStringLiteral("Dashboard"),
        QStringLiteral("Agents"),
    };
}

ChatMessageListModel* DesktopShellViewModel::chatMessages() {
    return &chatMessages_;
}

void DesktopShellViewModel::manageMemory(const QVariantMap& request) {
    if (controller_.isRemote()) {
        if (!controller_.remote()->ready()) {
            emit memoryManagementCompleted(
                {{"status", "Unavailable"}, {"requestTag", request.value("requestTag")}});
            return;
        }
        controller_.manageMemory(request);
    } else {
        const auto result = controller_.manageMemory(request);
        QTimer::singleShot(0, this, [this, result] { emit memoryManagementCompleted(result); });
    }
}

QStringList DesktopShellViewModel::memoryEntries() const {
    return controller_.memoryEntries();
}

QStringList DesktopShellViewModel::availableThemes() const { return core::AppSettings::availableThemes(); }

QString DesktopShellViewModel::themeName() const {
    return settings_.themeName();
}

void DesktopShellViewModel::setThemeName(const QString& themeName) {
    setProductSetting(QStringLiteral("appearance.theme"), themeName);
}

QString DesktopShellViewModel::configurationProfile() const {
    return settings_.configurationProfile();
}

void DesktopShellViewModel::setConfigurationProfile(const QString& configurationProfile) {
    settings_.setConfigurationProfile(configurationProfile);
}

QString DesktopShellViewModel::appLanguage() const {
    return settings_.appLanguage();
}

void DesktopShellViewModel::setAppLanguage(const QString& language) {
    setProductSetting(QStringLiteral("general.language"), language);
}

QStringList DesktopShellViewModel::availableLanguages() const {
    return settings_.availableLanguages();
}

QString DesktopShellViewModel::languageDisplayName(const QString& language) const {
    return settings_.languageDisplayName(language);
}

QVariantMap DesktopShellViewModel::getLocalModelDetails(const QString& modelName) const {
    QVariantMap result;
    const auto models = controller_.currentOllamaModels();
    const auto query = modelName.trimmed().toLower();
    for (const auto& model : models) {
        const auto current = model.name.trimmed().toLower();
        if (current == query || current.startsWith(query + QStringLiteral(":")) ||
            query.startsWith(current + QStringLiteral(":")) ||
            (query.length() > 3 && (current.contains(query) || query.contains(current)))) {
            result[QStringLiteral("name")] = model.name;
            result[QStringLiteral("sizeBytes")] = model.sizeBytes;
            result[QStringLiteral("sizeFormatted")] =
                model.sizeBytes > 0 ? QStringLiteral("%1 GB").arg(QString::number(
                                          model.sizeBytes / (1024.0 * 1024.0 * 1024.0), 'f', 1))
                                    : QStringLiteral("—");
            result[QStringLiteral("modifiedAt")] = model.modifiedAt;
            return result;
        }
    }
    return result;
}

bool DesktopShellViewModel::checkRuntimeInstalled(const QString& runtimeId) const {
    const auto lowerId = runtimeId.trimmed().toLower();
    if (lowerId == QLatin1String("ollama")) {
#if defined(Q_OS_MAC)
        return QFileInfo::exists(QStringLiteral("/Applications/Ollama.app")) ||
               QFileInfo::exists(QStringLiteral("/usr/local/bin/ollama")) ||
               QFileInfo::exists(QStringLiteral("/usr/bin/ollama"));
#elif defined(Q_OS_WIN)
        const QString localAppData =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        return QFileInfo::exists(
                   QDir(localAppData).filePath(QStringLiteral("Programs/Ollama/ollama.exe"))) ||
               QFileInfo::exists(QStringLiteral("C:\\Program Files\\Ollama\\ollama.exe")) ||
               QFileInfo::exists(
                   QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                       .filePath(QStringLiteral("AppData/Local/Programs/Ollama/ollama.exe")));
#else
        return QFileInfo::exists(QStringLiteral("/usr/local/bin/ollama")) ||
               QFileInfo::exists(QStringLiteral("/usr/bin/ollama")) ||
               QFileInfo::exists(QDir::homePath() + QStringLiteral("/.local/share/ollama"));
#endif
    } else if (lowerId == QLatin1String("lmstudio")) {
#if defined(Q_OS_MAC)
        return QFileInfo::exists(QStringLiteral("/Applications/LM Studio.app"));
#elif defined(Q_OS_WIN)
        const QString localAppData =
            QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        return QFileInfo::exists(
                   QDir(localAppData)
                       .filePath(QStringLiteral("Programs/lm-studio/LM Studio.exe"))) ||
               QFileInfo::exists(QStringLiteral("C:\\Program Files\\LM Studio\\LM Studio.exe")) ||
               QFileInfo::exists(
                   QDir(QStandardPaths::writableLocation(QStandardPaths::HomeLocation))
                       .filePath(QStringLiteral("AppData/Local/Programs/lm-studio/LM Studio.exe")));
#else
        return QFileInfo::exists(QStringLiteral("/usr/local/bin/lm-studio")) ||
               QFileInfo::exists(QDir::homePath() + QStringLiteral("/.local/share/lm-studio"));
#endif
    }
    return false;
}

bool DesktopShellViewModel::companionEnabled() const {
    return true;
}

void DesktopShellViewModel::setCompanionEnabled(bool enabled) {
    Q_UNUSED(enabled);
}

bool DesktopShellViewModel::companionAvailable() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .available;
}

bool DesktopShellViewModel::companionPaused() const {
    return companionPaused_;
}

bool DesktopShellViewModel::companionChatVisible() const {
    return companionChatVisible_;
}

void DesktopShellViewModel::setCompanionChatVisible(bool visible) {
    if (companionChatVisible_ == visible) {
        return;
    }
    companionChatVisible_ = visible;
    emit companionChatVisibleChanged();
}

void DesktopShellViewModel::registerMainWindow(quintptr winId) {
    if (taskbar_) {
        taskbar_->setWindowHandle(winId);
    }
}

void DesktopShellViewModel::toggleCompanionChat() {
    companionChatVisible_ = !companionChatVisible_;
    emit companionChatVisibleChanged();
    emit requestCompanionChat();
}

void DesktopShellViewModel::openCompanionChat() {
    if (!companionChatVisible_) {
        companionChatVisible_ = true;
        emit companionChatVisibleChanged();
    }
    emit requestCompanionChat();
}

void DesktopShellViewModel::hideCompanionChat() {
    if (companionChatVisible_) {
        companionChatVisible_ = false;
        emit companionChatVisibleChanged();
    }
}

void DesktopShellViewModel::setCompanionNativeAvailable(bool available) {
    if (available == companionNativeAvailable_) {
        return;
    }

    companionNativeAvailable_ = available;
    if (!available && companionPaused_) {
        companionPaused_ = false;
    }
    emit companionChanged();
}

void DesktopShellViewModel::setCompanionPaused(bool paused) {
    if (paused == companionPaused_) {
        return;
    }

    companionPaused_ = paused;
    emit companionChanged();
}

void DesktopShellViewModel::toggleCompanionPause() {
    setCompanionPaused(!companionPaused_);
}

QString DesktopShellViewModel::companionStatus() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .status;
}

QString DesktopShellViewModel::companionAvailability() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .availability;
}

QString DesktopShellViewModel::companionPlatformCapability() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .platformCapability;
}

QString DesktopShellViewModel::companionPermissionPosture() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .permissionPostureSummary;
}

QString DesktopShellViewModel::companionSafetyBoundary() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .safetyBoundarySummary;
}

QString DesktopShellViewModel::companionQuickCaptureSummary() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .quickCaptureSummary;
}

QStringList DesktopShellViewModel::companionActionSummaries() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .actionSummaries;
}

QStringList DesktopShellViewModel::companionPlatformSummaries() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .platformSummaries;
}

QStringList DesktopShellViewModel::companionTraceSummaries() const {
    return companionService_
        .summary(settings_.companionEnabled(), companionNativeAvailable_, companionPaused_)
        .traceSummaries;
}

bool DesktopShellViewModel::developerModeEnabled() const {
    return settings_.developerModeEnabled();
}

void DesktopShellViewModel::setDeveloperModeEnabled(bool enabled) {
    settings_.setDeveloperModeEnabled(enabled);
}

bool DesktopShellViewModel::agentAutonomousMode() const {
    return settings_.agentAutonomousMode();
}

void DesktopShellViewModel::setAgentAutonomousMode(bool enabled) {
    settings_.setAgentAutonomousMode(enabled);
    controller_.setAgentAutonomousMode(enabled);
    emit agentAutonomousModeChanged();
}

QString DesktopShellViewModel::updateCheckPolicy() const {
    return settings_.updateCheckPolicy();
}

void DesktopShellViewModel::setUpdateCheckPolicy(const QString& policy) {
    setProductSetting(QStringLiteral("network.update-check"), policy);
}

QString DesktopShellViewModel::updateCheckUrl() const {
    return settings_.updateCheckUrl();
}

void DesktopShellViewModel::setUpdateCheckUrl(const QString& url) {
    settings_.setUpdateCheckUrl(url);
}

void DesktopShellViewModel::setupBackgroundUpdateCheck() {
    if (!updateCheckTimer_) {
        updateCheckTimer_ = new QTimer(this);
        connect(updateCheckTimer_, &QTimer::timeout, this, [this]() {
            if (settings_.updateCheckPolicy() != QStringLiteral("Never")) {
                checkForUpdates();
            }
        });
    }
    updateCheckTimer_->stop();
    const auto policy = settings_.updateCheckPolicy();
    if (policy == QStringLiteral("Weekly")) {
        updateCheckTimer_->start(7 * 24 * 60 * 60 * 1000);
        checkForUpdates();
    } else if (policy == QStringLiteral("On Startup")) {
        QTimer::singleShot(3000, this, [this]() { checkForUpdates(); });
    }
}

QString DesktopShellViewModel::notificationPolicy() const {
    return settings_.notificationPolicy();
}

void DesktopShellViewModel::setNotificationPolicy(const QString& policy) {
    setProductSetting(QStringLiteral("notifications.policy"), policy);
}

bool DesktopShellViewModel::onboardingComplete() const {
    return settings_.onboardingComplete();
}

void DesktopShellViewModel::setOnboardingComplete(bool complete) {
    core::OnboardingService service(settings_, controller_.modelService());
    const auto result = complete ? service.finish() : service.reopen();
    onboardingErrorCode_ = result.errorCode;
    emit onboardingStateChanged();
}

int DesktopShellViewModel::onboardingStepIndex() const {
    return static_cast<int>(
        core::OnboardingService(settings_, controller_.modelService()).snapshot().step);
}

QString DesktopShellViewModel::onboardingProcessingMode() const {
    return core::OnboardingService(settings_, controller_.modelService()).snapshot().processingMode;
}

void DesktopShellViewModel::setOnboardingProcessingMode(const QString& mode) {
    const auto result =
        core::OnboardingService(settings_, controller_.modelService()).chooseProcessingMode(mode);
    onboardingErrorCode_ = result.errorCode;
    emit onboardingStateChanged();
}

QString DesktopShellViewModel::onboardingErrorText() const {
    return onboardingErrorCode_.isEmpty()
               ? QString{}
               : core::ProductMessages::present({onboardingErrorCode_, {}, {}});
}

bool DesktopShellViewModel::advanceOnboarding(bool skip) {
    const auto result =
        core::OnboardingService(settings_, controller_.modelService()).advance(skip);
    onboardingErrorCode_ = result.errorCode;
    emit onboardingStateChanged();
    return result.accepted;
}

bool DesktopShellViewModel::backOnboarding() {
    const auto result = core::OnboardingService(settings_, controller_.modelService()).back();
    onboardingErrorCode_ = result.errorCode;
    emit onboardingStateChanged();
    return result.accepted;
}

void DesktopShellViewModel::reopenOnboarding() {
    core::OnboardingService(settings_, controller_.modelService()).reopen();
    onboardingErrorCode_.clear();
    emit onboardingStateChanged();
}

QVariantMap DesktopShellViewModel::onboardingState() const {
    const auto state = core::OnboardingService(settings_, controller_.modelService()).snapshot();
    return {{QStringLiteral("version"), state.version},
            {QStringLiteral("stepId"), core::OnboardingService::stepId(state.step)},
            {QStringLiteral("stepIndex"), static_cast<int>(state.step)},
            {QStringLiteral("processingMode"), state.processingMode},
            {QStringLiteral("completedSteps"), state.completedSteps},
            {QStringLiteral("complete"), state.complete},
            {QStringLiteral("resumed"), state.resumed},
            {QStringLiteral("providerId"), state.providerId},
            {QStringLiteral("modelId"), state.modelId}};
}

static core::SettingsService composedSettingsService(core::AppSettings& settings,
                                                     const DesktopControllerBridge& controller) {
    return core::SettingsService(settings, controller.modelService(), controller.extensionService(),
                                 controller.audioSession(), controller.permissionService(),
                                 controller.conversationStore(), controller.memoryStore(),
                                 controller.mutableAgentRunStore(), controller.chatHistoryStore(),
                                 controller.modelOperations());
}

QVariantList DesktopShellViewModel::productSettings() const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("productSettings"), {}, true)
            .toMap()
            .value("value")
            .toList();
    auto service = composedSettingsService(settings_, controller_);
    QVariantList result;
    for (const auto& row : service.snapshots())
        result.append(QVariantMap{{QStringLiteral("id"), row.id},
                                  {QStringLiteral("section"), static_cast<int>(row.section)},
                                  {QStringLiteral("value"), row.value},
                                  {QStringLiteral("defaultValue"), row.defaultValue},
                                  {QStringLiteral("source"), row.source},
                                  {QStringLiteral("allowedValues"), row.allowedValues},
                                  {QStringLiteral("keywords"), row.keywords},
                                  {QStringLiteral("scope"), static_cast<int>(row.scope)},
                                  {QStringLiteral("enabled"), row.enabled},
                                  {QStringLiteral("restartRequired"), row.restartRequired},
                                  {QStringLiteral("sensitive"), row.sensitive},
                                  {QStringLiteral("unavailableReason"), row.unavailableReason}});
    return result;
}

QStringList DesktopShellViewModel::productSettingsSections() const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("productSettingsSections"), {}, true)
            .toMap()
            .value("value")
            .toStringList();
    return composedSettingsService(settings_, controller_).sectionIds();
}

QVariantMap DesktopShellViewModel::setProductSetting(const QString& id, const QVariant& value) {
    if (controller_.isRemote() && id != QStringLiteral("appearance.theme") &&
        id != QStringLiteral("general.language") && id != QStringLiteral("notifications.policy") &&
        id != QStringLiteral("network.update-check"))
        return controller_.remote()
            ->settingsService(QStringLiteral("setProductSetting"), {id, value}, false)
            .toMap();
    auto result = composedSettingsService(settings_, controller_).set(id, value);
    if (result.accepted) {
        if (id == QLatin1String("models.ollama-endpoint"))
            controller_.setOllamaEndpoint(settings_.ollamaEndpoint());
        else if (id == QLatin1String("models.lm-studio-endpoint"))
            controller_.setLmStudioEndpoint(settings_.lmStudioEndpoint());
        else if (id == QLatin1String("models.llama-cpp-endpoint"))
            controller_.setLlamaCppEndpoint(settings_.llamaCppEndpoint());
    }
    return {{QStringLiteral("accepted"), result.accepted},
            {QStringLiteral("code"), result.code},
            {QStringLiteral("field"), result.field},
            {QStringLiteral("parameters"), result.parameters.toVariantMap()}};
}

QVariantMap DesktopShellViewModel::resetProductSetting(const QString& id) {
    if (controller_.isRemote() && id != QStringLiteral("appearance.theme") &&
        id != QStringLiteral("general.language") && id != QStringLiteral("notifications.policy") &&
        id != QStringLiteral("network.update-check"))
        return controller_.remote()
            ->settingsService(QStringLiteral("resetProductSetting"), {id}, false)
            .toMap();
    auto result = composedSettingsService(settings_, controller_).reset(id);
    if (result.accepted) {
        if (id == QLatin1String("models.ollama-endpoint"))
            controller_.setOllamaEndpoint(settings_.ollamaEndpoint());
        else if (id == QLatin1String("models.lm-studio-endpoint"))
            controller_.setLmStudioEndpoint(settings_.lmStudioEndpoint());
        else if (id == QLatin1String("models.llama-cpp-endpoint"))
            controller_.setLlamaCppEndpoint(settings_.llamaCppEndpoint());
    }
    return {{QStringLiteral("accepted"), result.accepted},
            {QStringLiteral("code"), result.code},
            {QStringLiteral("field"), result.field}};
}

QVariantList DesktopShellViewModel::resetProductSettingsSection(int section) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("resetProductSettingsSection"), {section}, false)
            .toMap()
            .value("value")
            .toList();
    if (section < 0 || section > static_cast<int>(core::SettingSection::Advanced))
        return {};
    QVariantList result;
    for (const auto& item : composedSettingsService(settings_, controller_)
                                .resetSection(static_cast<core::SettingSection>(section)))
        result.append(QVariantMap{{QStringLiteral("accepted"), item.accepted},
                                  {QStringLiteral("code"), item.code},
                                  {QStringLiteral("field"), item.field}});
    if (section == static_cast<int>(core::SettingSection::Advanced)) {
        controller_.setOllamaEndpoint(settings_.ollamaEndpoint());
        controller_.setLmStudioEndpoint(settings_.lmStudioEndpoint());
        controller_.setLlamaCppEndpoint(settings_.llamaCppEndpoint());
    }
    return result;
}

QVariantMap DesktopShellViewModel::clearWorkspaceSettingOverride(const QString& workspaceId,
                                                                 const QString& key) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("clearWorkspaceSettingOverride"), {workspaceId, key},
                              false)
            .toMap();
    const auto result =
        composedSettingsService(settings_, controller_).clearWorkspaceOverride(workspaceId, key);
    return {{QStringLiteral("accepted"), result.accepted},
            {QStringLiteral("code"), result.code},
            {QStringLiteral("field"), result.field}};
}

QVariantMap DesktopShellViewModel::providerSettingsState(const QString& providerId) const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("providerSettingsState"), {providerId}, true)
            .toMap();
    return composedSettingsService(settings_, controller_).providerState(providerId).toVariantMap();
}

QVariantMap DesktopShellViewModel::extensionsHubSnapshot() const {
    auto* remote = controller_.remote();
    const bool connected = daemonConnected();
    const auto response = remote && connected && remote->ready()
        ? remote->settingsService("extensionsHubState", {}, true).toMap() : QVariantMap{};
    return ExtensionsHubPresentation::inventory(connected, remote && remote->ready(), response, {}, {});
}
QVariantMap DesktopShellViewModel::extensionsHubDetail(const QString& id) const {
    const auto snapshot = extensionsHubSnapshot();
    for (const auto& row : snapshot.value("entries").toList()) {
        if (row.toMap().value("id").toString() == id) {
            auto detail = row.toMap();
            detail["phase"] = "ready";
            return detail;
        }
    }
    return {{"phase", daemonConnected() ? "unavailable" : "disconnected"}};
}
QVariantMap DesktopShellViewModel::extensionsHubActionState() const {
    auto* remote = controller_.remote();
    return remote ? remote->extensionsHubActionState() : QVariantMap{{"phase", "unavailable"}};
}
bool DesktopShellViewModel::performExtensionsHubAction(const QString& id, const QString& action) {
    auto* remote = controller_.remote();
    return remote && remote->performExtensionsHubAction(id, action);
}
void DesktopShellViewModel::refreshExtensionsHub() {
    if (auto* remote = controller_.remote()) {
        remote->refreshExtensionsHub();
    }
}

QVariantMap DesktopShellViewModel::extensionSettingsState(const QString& extensionId) const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("extensionSettingsState"), {extensionId}, true)
            .toMap();
    return composedSettingsService(settings_, controller_)
        .extensionState(extensionId)
        .toVariantMap();
}

QVariantMap DesktopShellViewModel::performExtensionSettingsAction(const QString& extensionId,
                                                                  int action) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("performExtensionSettingsAction"),
                              {extensionId, action}, false)
            .toMap();
    const auto result =
        composedSettingsService(settings_, controller_).performExtensionAction(extensionId, action);
    return {{QStringLiteral("accepted"), result.accepted},
            {QStringLiteral("code"), result.code},
            {QStringLiteral("field"), result.field}};
}

QVariantMap DesktopShellViewModel::speechSettingsState() const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("speechSettingsState"), {}, true)
            .toMap();
    return composedSettingsService(settings_, controller_).speechState().toVariantMap();
}

QVariantMap DesktopShellViewModel::securitySettingsState() const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("securitySettingsState"), {}, true)
            .toMap();
    auto state = composedSettingsService(settings_, controller_).securityState().toVariantMap();
    state.insert(QStringLiteral("latestSandboxStatus"), controller_.latestSandboxStatus());
    state.insert(QStringLiteral("latestSandboxSummary"), controller_.latestSandboxSummary());
    return state;
}

QVariantMap DesktopShellViewModel::productPrivacyState() const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("productPrivacyState"), {}, true)
            .toMap();
    return composedSettingsService(settings_, controller_).privacyState().toVariantMap();
}

QVariantMap DesktopShellViewModel::productRecoveryState() const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("productRecoveryState"), {}, true)
            .toMap();
    return composedSettingsService(settings_, controller_).recoveryState().toVariantMap();
}

QObject* DesktopShellViewModel::backupManager() const { return backupManager_; }

QVariantMap DesktopShellViewModel::productBackupAvailability() const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("productBackupAvailability"), {}, true)
            .toMap();
    return composedSettingsService(settings_, controller_).backupAvailability().toVariantMap();
}

QVariantMap DesktopShellViewModel::exportProductBackup(const QStringList& domains) const {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("exportProductBackup"), {domains}, false)
            .toMap();
    const auto result = composedSettingsService(settings_, controller_).exportBackupJson(domains);
    return {{QStringLiteral("succeeded"), result.succeeded()},
            {QStringLiteral("error"), static_cast<int>(result.error)},
            {QStringLiteral("data"), result.data},
            {QStringLiteral("detail"), result.detail}};
}

QVariantMap DesktopShellViewModel::importProductBackup(const QByteArray& data,
                                                       const QStringList& domains, bool replace) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("importProductBackup"),
                              {data.toBase64(), domains, replace}, false)
            .toMap();
    const auto result = composedSettingsService(settings_, controller_)
                            .importBackupJson(data, domains,
                                              replace ? core::ImportMode::ReplaceSelectedDomains
                                                      : core::ImportMode::Merge);
    return {{QStringLiteral("succeeded"), result.succeeded()},
            {QStringLiteral("error"), static_cast<int>(result.error)},
            {QStringLiteral("detail"), result.detail}};
}

QVariantMap DesktopShellViewModel::clearProductData(const QString& domain) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("clearProductData"), {domain}, false)
            .toMap();
    auto service = composedSettingsService(settings_, controller_);
    core::SettingActionResult result;
    if (domain == QLatin1String("chat"))
        result = service.clearChatHistory();
    else if (domain == QLatin1String("memory"))
        result = service.clearMemory();
    else if (domain == QLatin1String("agentRuns"))
        result = service.clearAgentHistory();
    else if (domain == QLatin1String("temporaryData"))
        result = service.clearStaleTemporaryArtifacts();
    else if (domain == QLatin1String("cacheAndTemporaryData")) {
        const auto cleanup = service.clearCacheAndTemporaryData();
        return {
            {QStringLiteral("accepted"), !cleanup.value(QStringLiteral("partialFailure")).toBool()},
            {QStringLiteral("parameters"), cleanup.toVariantMap()}};
    } else
        return {{QStringLiteral("accepted"), false},
                {QStringLiteral("code"), QStringLiteral("UnknownDataDomain")}};
    return {{QStringLiteral("accepted"), result.accepted},
            {QStringLiteral("code"), result.code},
            {QStringLiteral("parameters"), result.parameters.toVariantMap()}};
}

QVariantMap DesktopShellViewModel::runProductMaintenance() {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("runProductMaintenance"), {}, false)
            .toMap();
    auto service = composedSettingsService(settings_, controller_);
    return service.runRetentionMaintenance().toVariantMap();
}

QVariantMap DesktopShellViewModel::resolveInterruptedModelOperation(const QString& operationId) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("resolveInterruptedModelOperation"), {operationId},
                              false)
            .toMap();
    const auto result = composedSettingsService(settings_, controller_)
                            .resolveInterruptedModelOperation(operationId);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}

QVariantMap DesktopShellViewModel::clearProductCredential(const QString& id) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("clearProductCredential"), {id}, false)
            .toMap();
    const auto result = composedSettingsService(settings_, controller_).clearCredential(id);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}

QVariantMap DesktopShellViewModel::setProductProviderCredential(const QString& providerId,
                                                                const QString& value) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("setProductProviderCredential"), {providerId, value},
                              false)
            .toMap();
    const auto result =
        composedSettingsService(settings_, controller_).setProviderCredential(providerId, value);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}

QVariantMap DesktopShellViewModel::setProductPluginCredential(const QString& pluginId,
                                                              const QString& credentialId,
                                                              const QString& value) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("setProductPluginCredential"),
                              {pluginId, credentialId, value}, false)
            .toMap();
    const auto result = composedSettingsService(settings_, controller_)
                            .setPluginCredential(pluginId, credentialId, value);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}

QVariantMap DesktopShellViewModel::clearProductPluginCredential(const QString& pluginId,
                                                                const QString& credentialId) {
    if (controller_.isRemote())
        return controller_.remote()
            ->settingsService(QStringLiteral("clearProductPluginCredential"),
                              {pluginId, credentialId}, false)
            .toMap();
    const auto result = composedSettingsService(settings_, controller_)
                            .clearPluginCredential(pluginId, credentialId);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}

QString DesktopShellViewModel::onboardingUseCase() const {
    return settings_.onboardingUseCase();
}

void DesktopShellViewModel::setOnboardingUseCase(const QString& useCase) {
    settings_.setOnboardingUseCase(useCase);
}

QString DesktopShellViewModel::onboardingAiProvider() const {
    return settings_.onboardingAiProvider();
}

void DesktopShellViewModel::setOnboardingAiProvider(const QString& provider) {
    settings_.setOnboardingAiProvider(provider);
}

QString DesktopShellViewModel::recoveryDraftText() const {
    return settings_.recoveryDraftText();
}

void DesktopShellViewModel::setRecoveryDraftText(const QString& text) {
    settings_.setRecoveryDraftText(text);
}

bool DesktopShellViewModel::reducedMotionEnabled() const {
    return settings_.reducedMotionEnabled();
}

void DesktopShellViewModel::setReducedMotionEnabled(bool enabled) {
    settings_.setReducedMotionEnabled(enabled);
}

bool DesktopShellViewModel::reducedTransparencyEnabled() const { return settings_.reducedTransparencyEnabled(); }
void DesktopShellViewModel::setReducedTransparencyEnabled(bool enabled) { settings_.setReducedTransparencyEnabled(enabled); }

bool DesktopShellViewModel::highContrastEnabled() const {
    return settings_.highContrastEnabled();
}

void DesktopShellViewModel::setHighContrastEnabled(bool enabled) {
    settings_.setHighContrastEnabled(enabled);
}

QString DesktopShellViewModel::uiDensity() const {
    return settings_.uiDensity();
}

void DesktopShellViewModel::setUiDensity(const QString& density) {
    settings_.setUiDensity(density);
}

QStringList DesktopShellViewModel::activityTimelineSummaries() const {
    QStringList lines;
    if (!conversationListCurrentTitle().isEmpty()) {
        lines << QStringLiteral("Chat session active - %1").arg(conversationListCurrentTitle());
    }
    lines << QStringLiteral("Memory entries - %1").arg(memoryEntryCount());
    lines << QStringLiteral("Notifications unread - %1").arg(unreadNotificationCount());
    lines << QStringLiteral("Theme - %1").arg(themeName());
    if (!conversationExportLastStatus().isEmpty()) {
        lines << QStringLiteral("Last export - %1").arg(conversationExportLastStatus());
    }
    const auto updateState = updateWorkflowState();
    if (!updateState.isEmpty()) {
        lines << QStringLiteral("Updates - %1").arg(updateState);
    }
    return lines;
}

QStringList DesktopShellViewModel::notificationCenterSummaries() const {
    return notificationFilteredSummaries();
}

QStringList DesktopShellViewModel::notificationCategories() const {
    return {QStringLiteral("All"),     QStringLiteral("Tasks"), QStringLiteral("Models"),
            QStringLiteral("Updates"), QStringLiteral("Brain"), QStringLiteral("Workspace"),
            QStringLiteral("Security")};
}

QStringList DesktopShellViewModel::notificationLifecycleSummaries() const {
    QStringList summaries;
    int pinned = 0;
    int unread = 0;
    int archived = 0;
    const auto notifications = notificationsFromJson(settings_.notificationCenterJson());
    for (const auto& value : notifications) {
        const auto item = value.toObject();
        pinned += item.value(QStringLiteral("pinned")).toBool() ? 1 : 0;
        unread += item.value(QStringLiteral("read")).toBool() ? 0 : 1;
        archived += item.value(QStringLiteral("archived")).toBool() ? 1 : 0;
    }
    summaries << QStringLiteral("Pinned: %1").arg(pinned)
              << QStringLiteral("Unread: %1").arg(unread)
              << QStringLiteral("Archived: %1").arg(archived)
              << QStringLiteral("Policy: %1").arg(settings_.notificationPolicy())
              << QStringLiteral("Persistence: local settings JSON");
    return summaries;
}

int DesktopShellViewModel::unreadNotificationCount() const {
    int count = 0;
    const auto notifications = notificationsFromJson(settings_.notificationCenterJson());
    for (const auto& value : notifications) {
        const auto item = value.toObject();
        if (!item.value(QStringLiteral("read")).toBool() &&
            !item.value(QStringLiteral("archived")).toBool()) {
            ++count;
        }
    }
    return count;
}

QStringList DesktopShellViewModel::notificationFilteredSummaries() const {
    QStringList summaries;
    const auto query = notificationSearchQuery_.trimmed().toLower();
    const auto filter = notificationCategoryFilter_;
    const auto notifications = notificationsFromJson(settings_.notificationCenterJson());
    for (const auto& value : notifications) {
        const auto item = value.toObject();
        const auto category = item.value(QStringLiteral("category")).toString();
        const auto line = notificationSummary(item);
        if (filter != QStringLiteral("All") && category != filter) {
            continue;
        }
        if (!query.isEmpty() && !line.toLower().contains(query) &&
            !item.value(QStringLiteral("body")).toString().toLower().contains(query)) {
            continue;
        }
        summaries.append(QString::fromUtf8(
            QJsonDocument(notificationJsonObject(item)).toJson(QJsonDocument::Compact)));
    }
    return summaries;
}

QString DesktopShellViewModel::notificationSearchQuery() const {
    return notificationSearchQuery_;
}

void DesktopShellViewModel::setNotificationSearchQuery(const QString& query) {
    if (query == notificationSearchQuery_) {
        return;
    }
    notificationSearchQuery_ = query;
    emit nativeExperienceChanged();
}

QString DesktopShellViewModel::notificationCategoryFilter() const {
    return notificationCategoryFilter_;
}

void DesktopShellViewModel::setNotificationCategoryFilter(const QString& category) {
    const auto selected =
        notificationCategories().contains(category) ? category : QStringLiteral("All");
    if (selected == notificationCategoryFilter_) {
        return;
    }
    notificationCategoryFilter_ = selected;
    emit nativeExperienceChanged();
}

QString DesktopShellViewModel::updateWorkflowState() const {
    return settings_.updateWorkflowState();
}

QString DesktopShellViewModel::lastAssetUrl() const {
    return lastAssetUrl_;
}

QStringList DesktopShellViewModel::releaseNotesSummaries() const {
    return {
        QStringLiteral("Sentinel %1 - Phase 52 packaging and 1.0 RC readiness")
            .arg(core::AppMetadata::version()),
        QStringLiteral("Packaging metadata, release presets, QA plan, and release checklist are "
                       "prepared without adding runtime authority."),
        QStringLiteral("Manual update check is explicit; no background polling, silent download, "
                       "or automatic install exists."),
    };
}

QStringList DesktopShellViewModel::aboutSentinelSummaries() const {
    auto summaries = core::AppMetadata::safeBuildSummaries();
    summaries.append(QStringLiteral("Qt: %1").arg(QString::fromLatin1(qVersion())));
    summaries.append(QStringLiteral("Copyright: %1").arg(core::AppMetadata::copyright()));
    summaries.append(
        QStringLiteral("License: project license and third-party notices in local docs"));
    summaries.append(QStringLiteral(
        "Docs: docs/legal/PRIVACY.md, docs/SECURITY.md, docs/UPDATES.md, docs/DIAGNOSTICS.md"));
    return summaries;
}

QStringList DesktopShellViewModel::accessibilitySummaries() const {
    return {
        QStringLiteral(
            "Keyboard navigation: enabled across shell, settings, command palette, and dialogs"),
        QStringLiteral("Focus indicators: tokenized focus borders on interactive controls"),
        QStringLiteral("Reduced motion: %1")
            .arg(settings_.reducedMotionEnabled() ? QStringLiteral("Enabled")
                                                  : QStringLiteral("Disabled")),
        QStringLiteral("High contrast: %1")
            .arg(settings_.highContrastEnabled() ? QStringLiteral("Enabled")
                                                 : QStringLiteral("Disabled")),
        QStringLiteral("UI density: %1").arg(settings_.uiDensity()),
        QStringLiteral("Screen reader labels: primary controls expose descriptive labels in QML"),
    };
}

QStringList DesktopShellViewModel::diagnosticsCenterSummaries() const {
    auto summaries = core::AppMetadata::safeBuildSummaries();
    summaries.append({
        QStringLiteral("Active provider: %1").arg(controller_.activeRuntimeProviderLabel()),
        QStringLiteral("Active model: %1").arg(controller_.activeRuntimeModelLabel()),
        QStringLiteral("Workspace count: %1").arg(workspaceIds().size()),
        QStringLiteral("Brain entries: %1").arg(memoryEntryCount()),
        QStringLiteral("Task statistics: %1").arg(controlledTaskDiagnosticsSummary()),
        QStringLiteral("Notification statistics: %1")
            .arg(notificationLifecycleSummaries().join(QStringLiteral(" / "))),
        QStringLiteral("Logs: no automatic log collection or upload"),
    });
    return summaries;
}

QStringList DesktopShellViewModel::exportPreviewSummaries() const {
    return {
        QStringLiteral("Source: %1").arg(exportPreviewSource_),
        QStringLiteral("Format: %1").arg(exportPreviewFormat_),
        QStringLiteral("Preview: conversations, Brain entries, task reports, and workspace "
                       "summaries support Markdown, TXT, JSON, and PDF."),
        QStringLiteral("Output: App-controlled export directory"),
        QStringLiteral("Privacy: export is foreground-only and user initiated"),
    };
}

QStringList DesktopShellViewModel::brainInsightSummaries() const {
    return {
        QStringLiteral("Recent activity: %1").arg(activityTimelineSummaries().value(0)),
        QStringLiteral("Workspace activity: %1").arg(brainWorkspaceSummaries().value(0)),
        QStringLiteral("Task completion trends: %1").arg(controlledTaskDiagnosticsSummary()),
        QStringLiteral("Model usage trends: active %1 / %2")
            .arg(controller_.activeRuntimeProviderLabel(), controller_.activeRuntimeModelLabel()),
        QStringLiteral("Visualization only: no analytics upload"),
    };
}

QStringList DesktopShellViewModel::recoveryReliabilitySummaries() const {
    return {
        QStringLiteral("Crash recovery draft: %1")
            .arg(settings_.recoveryDraftText().isEmpty() ? QStringLiteral("No draft saved")
                                                         : QStringLiteral("Draft available")),
        QStringLiteral("Recovery notifications: local in-app notices only"),
        QStringLiteral(
            "Unsaved work detection: composer drafts can be restored or discarded explicitly"),
        QStringLiteral("Safe shutdown summary: chats, Brain, settings, tasks, notifications, and "
                       "Local RAG remain separate local stores"),
    };
}

QStringList DesktopShellViewModel::productPolishSummaries() const {
    return {
        QStringLiteral("Home remains chat-first"),
        QStringLiteral("Conversation sidebar remains collapsible"),
        QStringLiteral("Smooth transitions respect reduced-motion preference"),
        QStringLiteral("Consistent spacing, responsive layouts, compact mode, large mode, empty, "
                       "loading, and error states are surfaced through shared QML components"),
    };
}

QString DesktopShellViewModel::responseProfileInstructions() const {
    return settings_.responseProfileInstructions();
}
void DesktopShellViewModel::setResponseProfileInstructions(const QString& instructions) {
    settings_.setResponseProfileInstructions(instructions);
    emit skillProfileChanged();
}
QString DesktopShellViewModel::selectedSkillProfile() const {
    return skillProfileService_.normalizedProfileId(settings_.selectedSkillProfile());
}

void DesktopShellViewModel::setSelectedSkillProfile(const QString& profileId) {
    settings_.setSelectedSkillProfile(skillProfileService_.normalizedProfileId(profileId));
}

QString DesktopShellViewModel::selectedSkillProfileName() const {
    return skillProfileService_.selectedProfile(settings_.selectedSkillProfile()).name;
}

QString DesktopShellViewModel::selectedSkillProfileSummary() const {
    return skillProfileService_.selectedProfile(settings_.selectedSkillProfile()).summary;
}

QString DesktopShellViewModel::selectedSkillProfileDescription() const {
    return skillProfileService_.selectedProfile(settings_.selectedSkillProfile()).description;
}

QString DesktopShellViewModel::selectedSkillProfileReadiness() const {
    return skillProfileService_.readiness(settings_.selectedSkillProfile()).status;
}

QString DesktopShellViewModel::selectedSkillProfilePolicyPosture() const {
    return skillProfileService_.selectedProfile(settings_.selectedSkillProfile()).policyPosture;
}

QStringList DesktopShellViewModel::skillProfileIds() const {
    QStringList ids;
    for (const auto& profile : skillProfileService_.availableProfiles()) {
        ids.append(profile.id);
    }
    return ids;
}

QStringList DesktopShellViewModel::skillProfileNames() const {
    QStringList names;
    for (const auto& profile : skillProfileService_.availableProfiles()) {
        names.append(profile.name);
    }
    return names;
}

QStringList DesktopShellViewModel::skillProfileSummaries() const {
    return skillProfileService_.profileSummaries();
}

QStringList DesktopShellViewModel::skillProfileCapabilitySummaries() const {
    return skillProfileService_.selectedProfile(settings_.selectedSkillProfile())
        .capabilitySummaries;
}

QStringList DesktopShellViewModel::skillProfileReadinessChecks() const {
    return skillProfileService_.readiness(settings_.selectedSkillProfile()).checks;
}

QStringList DesktopShellViewModel::skillProfileDeveloperDiagnostics() const {
    return skillProfileService_.readiness(settings_.selectedSkillProfile()).developerDiagnostics;
}

QString DesktopShellViewModel::selectedWorkspaceId() const {
    return workspaceService_.normalizedWorkspaceId(settings_.selectedWorkspaceId(),
                                                   settings_.workspaceCatalogJson());
}

QString DesktopShellViewModel::selectedWorkspaceRootPath() const {
    return workspaceService_
        .selectedWorkspace(selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .rootPath;
}

bool DesktopShellViewModel::workspaceRequiresLocalProvider() const {
    return controller_.currentWorkspaceRequiresLocal();
}

QVariantMap DesktopShellViewModel::currentWorkspaceProfile() const {
    const auto snapshot = controller_.currentWorkspaceProfile();
    return {{QStringLiteral("workspaceId"), snapshot.workspaceId},
            {QStringLiteral("presetId"), snapshot.presetId},
            {QStringLiteral("configured"), snapshot.configured.toVariantMap()},
            {QStringLiteral("effective"), snapshot.effective.toVariantMap()},
            {QStringLiteral("sources"), snapshot.sources.toVariantMap()},
            {QStringLiteral("statuses"), snapshot.statuses.toVariantMap()},
            {QStringLiteral("reasons"), snapshot.reasons.toVariantMap()},
            {QStringLiteral("unavailableReferences"), snapshot.unavailableReferences}};
}

QStringList DesktopShellViewModel::presetIds() const {
    QStringList ids;
    for (const auto& preset : workspaceService_.presets(settings_.workspaceProfilesJson()))
        ids.append(preset.id);
    return ids;
}

QStringList DesktopShellViewModel::presetNames() const {
    QStringList names;
    for (const auto& preset : workspaceService_.presets(settings_.workspaceProfilesJson()))
        names.append(preset.name);
    return names;
}

bool DesktopShellViewModel::setWorkspaceProfile(const QString& workspaceId, const QString& presetId,
                                                const QVariantMap& overrides) {
    if (workspaceService_.normalizedWorkspaceId(workspaceId, settings_.workspaceCatalogJson()) !=
        workspaceId)
        return false;
    const auto json =
        workspaceService_.updateProfile(settings_.workspaceProfilesJson(), workspaceId, presetId,
                                        QJsonObject::fromVariantMap(overrides));
    if (json.isEmpty())
        return false;
    settings_.clearStorageWriteError();
    settings_.setWorkspaceProfilesJson(json);
    return settings_.storageErrorCode().isEmpty();
}

QString DesktopShellViewModel::createPreset(const QString& name, const QVariantMap& preferences) {
    const auto before = workspaceService_.presets(settings_.workspaceProfilesJson());
    const auto json = workspaceService_.createPreset(settings_.workspaceProfilesJson(), name,
                                                     QJsonObject::fromVariantMap(preferences));
    if (json.isEmpty())
        return {};
    settings_.clearStorageWriteError();
    settings_.setWorkspaceProfilesJson(json);
    if (!settings_.storageErrorCode().isEmpty()) {
        return {};
    }
    const auto after = workspaceService_.presets(json);
    for (const auto& preset : after) {
        const auto existing = std::find_if(
            before.cbegin(), before.cend(),
            [&preset](const core::WorkspacePreset& item) { return item.id == preset.id; });
        if (existing == before.cend())
            return preset.id;
    }
    return {};
}

bool DesktopShellViewModel::renamePreset(const QString& presetId, const QString& name) {
    const auto json =
        workspaceService_.renamePreset(settings_.workspaceProfilesJson(), presetId, name);
    if (json.isEmpty())
        return false;
    settings_.clearStorageWriteError();
    settings_.setWorkspaceProfilesJson(json);
    return settings_.storageErrorCode().isEmpty();
}

bool DesktopShellViewModel::updatePreset(const QString& presetId, const QVariantMap& preferences) {
    const auto json = workspaceService_.updatePreset(settings_.workspaceProfilesJson(), presetId,
                                                     QJsonObject::fromVariantMap(preferences));
    if (json.isEmpty())
        return false;
    settings_.clearStorageWriteError();
    settings_.setWorkspaceProfilesJson(json);
    return settings_.storageErrorCode().isEmpty();
}

QString DesktopShellViewModel::duplicatePreset(const QString& presetId) {
    const auto before = workspaceService_.presets(settings_.workspaceProfilesJson());
    const auto json =
        workspaceService_.duplicatePreset(settings_.workspaceProfilesJson(), presetId);
    if (json.isEmpty())
        return {};
    settings_.clearStorageWriteError();
    settings_.setWorkspaceProfilesJson(json);
    if (!settings_.storageErrorCode().isEmpty()) {
        return {};
    }
    const auto after = workspaceService_.presets(json);
    for (const auto& preset : after) {
        const auto existing = std::find_if(
            before.cbegin(), before.cend(),
            [&preset](const core::WorkspacePreset& item) { return item.id == preset.id; });
        if (existing == before.cend())
            return preset.id;
    }
    return {};
}

bool DesktopShellViewModel::deletePreset(const QString& presetId) {
    const auto json = workspaceService_.deletePreset(settings_.workspaceProfilesJson(), presetId);
    if (json.isEmpty())
        return false;
    settings_.clearStorageWriteError();
    settings_.setWorkspaceProfilesJson(json);
    return settings_.storageErrorCode().isEmpty();
}

void DesktopShellViewModel::setSelectedWorkspaceId(const QString& workspaceId) {
    settings_.setSelectedWorkspaceId(
        workspaceService_.normalizedWorkspaceId(workspaceId, settings_.workspaceCatalogJson()));
}

bool DesktopShellViewModel::setWorkspaceRoot(const QString& workspaceId, const QString& rootPath) {
    auto result =
        workspaceService_.setWorkspaceRoot(settings_.workspaceCatalogJson(), workspaceId, rootPath);
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    if (result.success) {
        const auto selected = workspaceService_.normalizedWorkspaceId(
            settings_.selectedWorkspaceId(), result.catalogJson);
        result.success = settings_.persistWorkspaceState(result.catalogJson, selected);
        if (!result.success) {
            workspaceLastActionStatus_ = settings_.storageErrorCode();
            workspaceLastActionSummary_ = workspaceLastActionStatus_;
        }
    }
    emit workspaceChanged();
    return result.success;
}

bool DesktopShellViewModel::openWorkspaceFolder(const QString& rootPath) {
    const QFileInfo folder(rootPath.trimmed());
    if (!folder.isDir() || !folder.isReadable()) {
        workspaceLastActionStatus_ = QStringLiteral("Refused");
        workspaceLastActionSummary_ =
            QStringLiteral("Workspace folder must be an existing readable directory.");
        emit workspaceChanged();
        return false;
    }

    const auto active = workspaceService_.selectedWorkspace(selectedWorkspaceId(),
                                                            settings_.workspaceCatalogJson());
    if (active.kind != QLatin1String("Built-in template"))
        return setWorkspaceRoot(active.id, folder.absoluteFilePath());

    const auto name = folder.fileName().trimmed().isEmpty() ? QStringLiteral("Workspace")
                                                            : folder.fileName().trimmed();
    const auto created = workspaceService_.createWorkspace(settings_.workspaceCatalogJson(), name,
                                                           QStringLiteral("Coding"));
    if (!created.success) {
        workspaceLastActionStatus_ = created.status;
        workspaceLastActionSummary_ = created.summary;
        emit workspaceChanged();
        return false;
    }

    const auto rooted = workspaceService_.setWorkspaceRoot(
        created.catalogJson, created.selectedWorkspaceId, folder.absoluteFilePath());
    workspaceLastActionStatus_ = rooted.status;
    workspaceLastActionSummary_ =
        rooted.success
            ? QStringLiteral("Opened workspace folder: %1").arg(folder.absoluteFilePath())
            : rooted.summary;
    if (!rooted.success) {
        emit workspaceChanged();
        return false;
    }
    if (!settings_.persistWorkspaceState(rooted.catalogJson, created.selectedWorkspaceId)) {
        workspaceLastActionStatus_ = settings_.storageErrorCode();
        workspaceLastActionSummary_ = workspaceLastActionStatus_;
        emit workspaceChanged();
        return false;
    }
    emit workspaceChanged();
    return true;
}

bool DesktopShellViewModel::clearWorkspaceRoot() {
    const auto active = workspaceService_.selectedWorkspace(selectedWorkspaceId(),
                                                            settings_.workspaceCatalogJson());
    if (active.rootPath.isEmpty()) {
        workspaceLastActionStatus_ = QStringLiteral("Refused");
        workspaceLastActionSummary_ = QStringLiteral("The active workspace has no folder root.");
        emit workspaceChanged();
        return false;
    }
    return setWorkspaceRoot(active.id, {});
}

QString DesktopShellViewModel::selectedWorkspaceName() const {
    return workspaceService_
        .selectedWorkspace(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .name;
}

QString DesktopShellViewModel::selectedWorkspaceAccessState() const {
    return workspaceService_
        .selectedWorkspace(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .accessState;
}

QString DesktopShellViewModel::workspacePermissionPosture() const {
    return workspaceService_
        .selectedWorkspace(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .permissionPosture;
}

QString DesktopShellViewModel::selectedWorkspaceRootSummary() const {
    return workspaceService_
        .selectedWorkspace(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .rootSummary;
}

QString DesktopShellViewModel::workspaceReadinessStatus() const {
    return workspaceService_
        .readiness(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .status;
}

QString DesktopShellViewModel::workspaceReadinessSummary() const {
    return workspaceService_
        .readiness(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .summary;
}

QString DesktopShellViewModel::workspacePermissionSummary() const {
    return workspaceService_
        .selectedWorkspace(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .permissionSummary;
}

QStringList DesktopShellViewModel::workspaceIds() const {
    QStringList ids;
    for (const auto& workspace :
         workspaceService_.availableWorkspaces(settings_.workspaceCatalogJson())) {
        ids.append(workspace.id);
    }
    return ids;
}

QStringList DesktopShellViewModel::workspaceNames() const {
    QStringList names;
    for (const auto& workspace :
         workspaceService_.availableWorkspaces(settings_.workspaceCatalogJson())) {
        names.append(workspace.name);
    }
    return names;
}

QStringList DesktopShellViewModel::workspaceSummaries() const {
    return workspaceService_.workspaceSummaries(settings_.workspaceCatalogJson());
}

QStringList DesktopShellViewModel::workspacePermissionPostures() const {
    return workspaceService_.permissionPostures();
}

QStringList DesktopShellViewModel::workspaceActionPlaceholders() const {
    return workspaceService_.actionPlaceholders();
}

QStringList DesktopShellViewModel::workspaceReadinessChecks() const {
    return workspaceService_
        .readiness(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .checks;
}

QStringList DesktopShellViewModel::workspaceBoundaryDiagnostics() const {
    return workspaceService_
        .readiness(settings_.selectedWorkspaceId(), settings_.workspaceCatalogJson())
        .boundaryDiagnostics;
}

QStringList DesktopShellViewModel::workspaceTemplateNames() const {
    return workspaceService_.builtInTemplateNames();
}

QString DesktopShellViewModel::workspaceLastActionStatus() const {
    return workspaceLastActionStatus_;
}

QString DesktopShellViewModel::workspaceLastActionSummary() const {
    return workspaceLastActionSummary_;
}

QStringList DesktopShellViewModel::attachmentSummaries() const {
    QStringList summaries;
    for (const auto& attachment : attachments_) {
        summaries.append(core::ragDocumentSummary(attachment));
    }
    return summaries;
}

QString DesktopShellViewModel::attachmentStatus() const {
    return attachments_.isEmpty() ? QStringLiteral("No Attachments")
                                  : QStringLiteral("%1 attachment(s)").arg(attachments_.size());
}

QString DesktopShellViewModel::attachmentPreviewSummary() const {
    if (attachments_.isEmpty()) {
        return QStringLiteral("No file attached. Drag, browse, or paste explicitly to attach.");
    }
    return core::ragDocumentSummary(attachments_.last());
}

QStringList DesktopShellViewModel::fileChatActionSummaries() const {
    return {
        QStringLiteral("Summarize attached file"),
        QStringLiteral("Ask questions about attached file"),
        QStringLiteral("Extract key points"),
        QStringLiteral("Translate selected attachment"),
        QStringLiteral("Export file-chat results"),
    };
}

bool DesktopShellViewModel::localKnowledgeBaseEnabled() const {
    return settings_.localKnowledgeBaseEnabled();
}

void DesktopShellViewModel::setLocalKnowledgeBaseEnabled(bool enabled) {
    settings_.setLocalKnowledgeBaseEnabled(enabled);
}

QString DesktopShellViewModel::attachmentBehavior() const {
    return settings_.attachmentBehavior();
}

void DesktopShellViewModel::setAttachmentBehavior(const QString& behavior) {
    settings_.setAttachmentBehavior(behavior);
}

QString DesktopShellViewModel::exportDefaultFormat() const {
    return settings_.exportDefaultFormat();
}

void DesktopShellViewModel::setExportDefaultFormat(const QString& format) {
    settings_.setExportDefaultFormat(format);
}

bool DesktopShellViewModel::exportIncludeTimestamps() const {
    return settings_.exportIncludeTimestamps();
}

void DesktopShellViewModel::setExportIncludeTimestamps(bool enabled) {
    settings_.setExportIncludeTimestamps(enabled);
}

bool DesktopShellViewModel::exportIncludeCitations() const {
    return settings_.exportIncludeCitations();
}

void DesktopShellViewModel::setExportIncludeCitations(bool enabled) {
    settings_.setExportIncludeCitations(enabled);
}

bool DesktopShellViewModel::exportAnonymizeNames() const {
    return settings_.exportAnonymizeNames();
}

void DesktopShellViewModel::setExportAnonymizeNames(bool enabled) {
    settings_.setExportAnonymizeNames(enabled);
}

bool DesktopShellViewModel::exportIncludeModelMetadata() const {
    return settings_.exportIncludeModelMetadata();
}

void DesktopShellViewModel::setExportIncludeModelMetadata(bool enabled) {
    settings_.setExportIncludeModelMetadata(enabled);
}

QString DesktopShellViewModel::localKnowledgeBaseStatus() const {
    if (!settings_.localKnowledgeBaseEnabled()) {
        return QStringLiteral("Disabled");
    }
    return localRagStore_ && localRagStore_->isAvailable() ? QStringLiteral("Enabled / Manual Only")
                                                           : QStringLiteral("Unavailable");
}

QStringList DesktopShellViewModel::knowledgeBaseDocumentSummaries() const {
    if (!localRagStore_) {
        return {};
    }
    QStringList summaries;
    for (const auto& document : localRagStore_->documents(selectedWorkspaceId())) {
        summaries.append(core::ragDocumentSummary(document));
    }
    return summaries;
}

QStringList DesktopShellViewModel::recentRetrievalSummaries() const {
    if (!localRagStore_) {
        return {};
    }
    QStringList summaries;
    for (const auto& retrieval : localRagStore_->recentRetrievals(selectedWorkspaceId())) {
        summaries.append(core::ragRetrievalSummary(retrieval));
    }
    return summaries;
}

QStringList DesktopShellViewModel::retrievalExplainabilitySummaries() const {
    if (!settings_.retrievalExplainabilityEnabled()) {
        return {QStringLiteral("Retrieval explainability disabled by user setting.")};
    }
    return {
        QStringLiteral("Source document: shown for each retrieval result"),
        QStringLiteral("Section/chunk reference: shown as manual chunk metadata"),
        QStringLiteral("Relevance metadata: shown as bounded local score metadata"),
    };
}

QStringList DesktopShellViewModel::brainWorkspaceSummaries() const {
    const auto* controlled = controller_.controlledTasks();
    const auto taskCount = [controlled, this](const QString& state) {
        return controlled
                   ? controlled->presentation()
                         .timelineSummaries(controlled->tasks(), selectedWorkspaceId(), state)
                         .size()
                   : 0;
    };
    return {
        QStringLiteral("Workspace Timeline - %1 / %2")
            .arg(selectedWorkspaceName(), workspaceLastActionSummary_),
        QStringLiteral("Attached Documents - %1").arg(attachmentStatus()),
        QStringLiteral("Knowledge Base Summary - %1 / %2 document(s)")
            .arg(localKnowledgeBaseStatus())
            .arg(knowledgeBaseDocumentSummaries().size()),
        QStringLiteral("Recent Retrievals - %1 record(s)").arg(recentRetrievalSummaries().size()),
        QStringLiteral("Planned Tasks - %1").arg(taskCount(QStringLiteral("pending approval"))),
        QStringLiteral("Completed Tasks - %1").arg(taskCount(QStringLiteral("completed"))),
        QStringLiteral("Failed Tasks - %1").arg(taskCount(QStringLiteral("failed"))),
        QStringLiteral("Cancelled Tasks - %1").arg(taskCount(QStringLiteral("cancelled"))),
    };
}

QStringList DesktopShellViewModel::exportCenterSummaries() const {
    auto summaries = QStringList{
        QStringLiteral("Chats - Markdown, PDF, TXT, DOCX, JSON"),
        QStringLiteral("Workspace summaries - %1 default").arg(settings_.exportDefaultFormat()),
        QStringLiteral("Document summaries - citations %1")
            .arg(settings_.exportIncludeCitations() ? QStringLiteral("included")
                                                    : QStringLiteral("excluded")),
        QStringLiteral("Retrieval reports - timestamps %1 / anonymize names %2 / model metadata %3")
            .arg(settings_.exportIncludeTimestamps() ? QStringLiteral("included")
                                                     : QStringLiteral("excluded"),
                 settings_.exportAnonymizeNames() ? QStringLiteral("on") : QStringLiteral("off"),
                 settings_.exportIncludeModelMetadata() ? QStringLiteral("included")
                                                        : QStringLiteral("excluded")),
    };
    summaries.append(controlledTaskExportSummaries());
    return summaries;
}

QStringList DesktopShellViewModel::privacyCenterSummaries() const {
    return {
        QStringLiteral("Knowledge Base: %1")
            .arg(settings_.localKnowledgeBaseEnabled() ? QStringLiteral("Enabled")
                                                       : QStringLiteral("Disabled")),
        QStringLiteral("Indexing: Manual Only"),
        QStringLiteral("Document Scope: Workspace Only"),
        QStringLiteral("Cloud Retrieval: Disabled"),
        QStringLiteral("Telemetry: Disabled"),
        QStringLiteral("Hidden filesystem scanning: Disabled"),
        QStringLiteral("Autonomous agents: Disabled"),
        QStringLiteral("Hidden task execution: Disabled"),
        QStringLiteral("Cloud activation from tasks: Disabled"),
    };
}

QString DesktopShellViewModel::createWorkspace(const QString& name, const QString& templateName) {
    auto result =
        workspaceService_.createWorkspace(settings_.workspaceCatalogJson(), name, templateName);
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    if (result.success) {
        result.success =
            settings_.persistWorkspaceState(result.catalogJson, result.selectedWorkspaceId);
        if (!result.success) {
            workspaceLastActionStatus_ = settings_.storageErrorCode();
            workspaceLastActionSummary_ = workspaceLastActionStatus_;
        }
    }
    emit workspaceChanged();
    return result.success ? result.selectedWorkspaceId : QString();
}

bool DesktopShellViewModel::renameWorkspace(const QString& workspaceId, const QString& name) {
    auto result =
        workspaceService_.renameWorkspace(settings_.workspaceCatalogJson(), workspaceId, name);
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    if (result.success) {
        const auto selected = workspaceService_.normalizedWorkspaceId(
            settings_.selectedWorkspaceId(), result.catalogJson);
        result.success = settings_.persistWorkspaceState(result.catalogJson, selected);
        if (!result.success) {
            workspaceLastActionStatus_ = settings_.storageErrorCode();
            workspaceLastActionSummary_ = workspaceLastActionStatus_;
        }
    }
    emit workspaceChanged();
    return result.success;
}

bool DesktopShellViewModel::archiveWorkspace(const QString& workspaceId) {
    auto result = workspaceService_.archiveWorkspace(settings_.workspaceCatalogJson(), workspaceId);
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    if (result.success) {
        const auto selected = workspaceService_.normalizedWorkspaceId(
            settings_.selectedWorkspaceId(), result.catalogJson);
        result.success = settings_.persistWorkspaceState(result.catalogJson, selected);
        if (!result.success) {
            workspaceLastActionStatus_ = settings_.storageErrorCode();
            workspaceLastActionSummary_ = workspaceLastActionStatus_;
        }
    }
    emit workspaceChanged();
    return result.success;
}

bool DesktopShellViewModel::deleteWorkspace(const QString& workspaceId) {
    auto result = workspaceService_.deleteWorkspace(settings_.workspaceCatalogJson(), workspaceId,
                                                    settings_.selectedWorkspaceId());
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    if (result.success) {
        result.success =
            settings_.persistWorkspaceState(result.catalogJson, result.selectedWorkspaceId);
        if (!result.success) {
            workspaceLastActionStatus_ = settings_.storageErrorCode();
            workspaceLastActionSummary_ = workspaceLastActionStatus_;
        }
    }
    emit workspaceChanged();
    return result.success;
}

QString DesktopShellViewModel::duplicateWorkspace(const QString& workspaceId) {
    auto result =
        workspaceService_.duplicateWorkspace(settings_.workspaceCatalogJson(), workspaceId);
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    if (result.success) {
        result.success =
            settings_.persistWorkspaceState(result.catalogJson, result.selectedWorkspaceId);
        if (!result.success) {
            workspaceLastActionStatus_ = settings_.storageErrorCode();
            workspaceLastActionSummary_ = workspaceLastActionStatus_;
        }
    }
    emit workspaceChanged();
    return result.success ? result.selectedWorkspaceId : QString();
}

bool DesktopShellViewModel::attachFileToChat(const QString& fileValue) {
    const QUrl url(fileValue);
    const auto filePath = url.isLocalFile() ? url.toLocalFile() : fileValue;
    const QFileInfo info(filePath);
    attachmentError_.clear();
    if (!info.exists() || !info.isFile()) {
        workspaceLastActionStatus_ = QStringLiteral("Refused");
        workspaceLastActionSummary_ = QStringLiteral("Attachment requires one explicit file.");
        attachmentError_ = workspaceLastActionSummary_; emit attachmentChanged();
        emit workspaceChanged();
        return false;
    }
    const auto type = fileTypeForPath(filePath);
    if (!supportedAttachmentType(type)) {
        workspaceLastActionStatus_ = QStringLiteral("Refused");
        workspaceLastActionSummary_ = QStringLiteral("Unsupported attachment type.");
        attachmentError_ = workspaceLastActionSummary_; emit attachmentChanged();
        emit workspaceChanged();
        return false;
    }
    if (attachments_.size() >= 4 || info.size() > 4 * 1024 * 1024) {
        attachmentError_ = tr("Attach at most four files, each up to 4 MB."); emit attachmentChanged(); return false;
    }
    const auto id = attachmentId(selectedWorkspaceId(), info.fileName(), info.size());
    attachmentPaths_.insert(id, info.absoluteFilePath());
    attachments_.append({id,
                         selectedWorkspaceId(), info.fileName(), type, info.size(),
                         QStringLiteral("Attached Explicitly"),
                         QStringLiteral("Explicit user attachment; no folder import.")});
    workspaceLastActionStatus_ = QStringLiteral("Document Attached");
    workspaceLastActionSummary_ = QStringLiteral("Attached %1.").arg(info.fileName());
    emit attachmentChanged();
    emit workspaceChanged();
    return true;
}

bool DesktopShellViewModel::pasteAttachment(const QString& name, const QString& text) {
    const auto fileName =
        name.trimmed().isEmpty() ? QStringLiteral("pasted-attachment.txt") : name.trimmed();
    const auto type = fileTypeForPath(fileName);
    if (!supportedAttachmentType(type) || text.trimmed().isEmpty()) {
        workspaceLastActionStatus_ = QStringLiteral("Refused");
        workspaceLastActionSummary_ =
            QStringLiteral("Paste attachment requires supported name and text.");
        emit workspaceChanged();
        return false;
    }
    attachments_.append({attachmentId(selectedWorkspaceId(), fileName, text.size()),
                         selectedWorkspaceId(), fileName, type, text.toUtf8().size(),
                         QStringLiteral("Pasted Explicitly"),
                         QStringLiteral("Explicit paste attachment; no background processing.")});
    workspaceLastActionStatus_ = QStringLiteral("Document Attached");
    workspaceLastActionSummary_ = QStringLiteral("Pasted %1.").arg(fileName);
    emit attachmentChanged();
    emit workspaceChanged();
    return true;
}

bool DesktopShellViewModel::removeAttachment(const QString& attachmentIdValue) {
    for (qsizetype index = 0; index < attachments_.size(); ++index) {
        if (attachments_.at(index).id == attachmentIdValue.trimmed()) {
            attachmentPaths_.remove(attachments_.at(index).id);
            attachments_.removeAt(index);
            workspaceLastActionStatus_ = QStringLiteral("Attachment Removed");
            workspaceLastActionSummary_ = QStringLiteral("Removed attachment.");
            emit attachmentChanged();
            emit workspaceChanged();
            return true;
        }
    }
    return false;
}

void DesktopShellViewModel::clearAttachments() {
    attachmentPaths_.clear();
    attachmentSendPending_ = false;
    attachmentError_.clear();
    attachments_.clear();
    workspaceLastActionStatus_ = QStringLiteral("Attachments Cleared");
    workspaceLastActionSummary_ = QStringLiteral("Cleared all attachments.");
    emit attachmentChanged();
    emit workspaceChanged();
}

bool DesktopShellViewModel::replaceAttachment(const QString& attachmentIdValue,
                                              const QString& filePath) {
    if (!removeAttachment(attachmentIdValue)) {
        return false;
    }
    return attachFileToChat(filePath);
}

bool DesktopShellViewModel::addKnowledgeBaseDocument(const QString& filePath) {
    if (!settings_.localKnowledgeBaseEnabled() || !localRagStore_) {
        workspaceLastActionStatus_ = QStringLiteral("Refused");
        workspaceLastActionSummary_ = QStringLiteral("Enable the workspace knowledge base first.");
        emit workspaceChanged();
        return false;
    }
    const QFileInfo info(filePath);
    const auto type = fileTypeForPath(filePath);
    if (!info.exists() || !info.isFile() || !supportedAttachmentType(type)) {
        workspaceLastActionStatus_ = QStringLiteral("Indexing Failed");
        workspaceLastActionSummary_ =
            QStringLiteral("Knowledge base accepts explicit supported files only.");
        emit workspaceChanged();
        return false;
    }
    const auto result = localRagStore_->addDocument(
        {attachmentId(selectedWorkspaceId(), info.fileName(), info.size()), selectedWorkspaceId(),
         info.fileName(), type, info.size(), QStringLiteral("Added / Not Indexed"),
         QStringLiteral("Workspace-only explicit document.")});
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    emit workspaceChanged();
    return result.success;
}

bool DesktopShellViewModel::removeKnowledgeBaseDocument(const QString& documentId) {
    if (!localRagStore_) {
        return false;
    }
    const auto result = localRagStore_->removeDocument(selectedWorkspaceId(), documentId);
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    emit workspaceChanged();
    return result.success;
}

bool DesktopShellViewModel::reindexKnowledgeBase() {
    if (!settings_.localKnowledgeBaseEnabled() || !localRagStore_) {
        workspaceLastActionStatus_ = QStringLiteral("Refused");
        workspaceLastActionSummary_ =
            QStringLiteral("Manual re-index requires an enabled knowledge base.");
        emit workspaceChanged();
        return false;
    }
    const auto result = localRagStore_->markReindexed(selectedWorkspaceId());
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    emit workspaceChanged();
    return result.success;
}

bool DesktopShellViewModel::clearKnowledgeBase() {
    if (!localRagStore_) {
        return false;
    }
    const auto result = localRagStore_->clearWorkspace(selectedWorkspaceId());
    workspaceLastActionStatus_ = result.status;
    workspaceLastActionSummary_ = result.summary;
    emit workspaceChanged();
    return result.success;
}

QString DesktopShellViewModel::defaultPermissionPolicyState() const {
    return permissionPolicyService_.normalizedState(settings_.defaultPermissionPolicyState());
}

void DesktopShellViewModel::setDefaultPermissionPolicyState(const QString& state) {
    setProductSetting(QStringLiteral("privacy.permission-policy"), state);
}

QString DesktopShellViewModel::permissionPolicyStatus() const {
    return permissionPolicyService_.registrySummary(settings_.defaultPermissionPolicyState())
        .status;
}

QString DesktopShellViewModel::permissionPolicySummary() const {
    return permissionPolicyService_.registrySummary(settings_.defaultPermissionPolicyState())
        .summary;
}

QStringList DesktopShellViewModel::permissionPolicyStateLabels() const {
    return permissionPolicyService_.permissionStateLabels();
}

QStringList DesktopShellViewModel::permissionPolicyDomainIds() const {
    return permissionPolicyService_.permissionDomainIds();
}

QStringList DesktopShellViewModel::permissionPolicyDomainNames() const {
    return permissionPolicyService_.permissionDomainNames();
}

QStringList DesktopShellViewModel::permissionPolicyDomainSummaries() const {
    return permissionPolicyService_.registrySummary(settings_.defaultPermissionPolicyState())
        .domainSummaries;
}

QStringList DesktopShellViewModel::permissionPolicyDeveloperDiagnostics() const {
    return permissionPolicyService_.registrySummary(settings_.defaultPermissionPolicyState())
        .developerDiagnostics;
}

QString DesktopShellViewModel::toolGatewayStatus() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .status;
}

QString DesktopShellViewModel::toolGatewaySummary() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .summary;
}

QString DesktopShellViewModel::toolGatewayPermissionPosture() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .permissionPosture;
}

int DesktopShellViewModel::toolGatewayToolCount() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .toolCount;
}

int DesktopShellViewModel::toolGatewayMetadataSafeCount() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .metadataSafeCount;
}

int DesktopShellViewModel::toolGatewayUnavailableCount() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .unavailableCount;
}

int DesktopShellViewModel::toolGatewayRefusedCount() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .refusedCount;
}

QStringList DesktopShellViewModel::toolGatewayToolSummaries() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .toolSummaries;
}

QStringList DesktopShellViewModel::toolGatewayDeveloperDiagnostics() const {
    return toolExecutionGateway_
        .registrySummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_)
        .developerDiagnostics;
}

QString DesktopShellViewModel::agentRuntimeStatus() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .status;
}

QString DesktopShellViewModel::agentRuntimeSummary() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .summary;
}

QString DesktopShellViewModel::agentRuntimeApprovalPosture() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .approvalPosture;
}

int DesktopShellViewModel::agentRuntimeAgentCount() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .agentCount;
}

int DesktopShellViewModel::agentRuntimeReadyAgentCount() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .readyAgentCount;
}

int DesktopShellViewModel::agentRuntimeRefusedAgentCount() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .refusedAgentCount;
}

QStringList DesktopShellViewModel::agentRuntimeAgentSummaries() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .agentSummaries;
}

QStringList DesktopShellViewModel::agentRuntimeReadinessSummaries() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .readinessSummaries;
}

QStringList DesktopShellViewModel::agentRuntimeDeveloperDiagnostics() const {
    return agentRuntimeService_
        .runtimeSummary(settings_.defaultPermissionPolicyState(), permissionPolicyService_,
                        toolExecutionGateway_, skillProfileService_,
                        settings_.selectedSkillProfile(), workspaceService_,
                        settings_.selectedWorkspaceId())
        .developerDiagnostics;
}

QString DesktopShellViewModel::agentPlanId() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .planId;
}

QString DesktopShellViewModel::agentPlanGoalSummary() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .goalSummary;
}

QStringList DesktopShellViewModel::agentPlanSteps() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .orderedPlanSteps;
}

QStringList DesktopShellViewModel::agentPlanRequiredTools() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .requiredTools;
}

QStringList DesktopShellViewModel::agentPlanRequiredPermissions() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .requiredPermissions;
}

QString DesktopShellViewModel::agentPlanEstimatedRisk() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .estimatedRisk;
}

QString DesktopShellViewModel::agentPlanApprovalState() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .approvalState;
}

QString DesktopShellViewModel::agentPlanRefusalReason() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .refusalReason;
}

QStringList DesktopShellViewModel::agentPlanDiagnostics() const {
    return agentRuntimeService_
        .previewPlan(QStringLiteral("Review the current goal and propose safe next steps."),
                     QStringLiteral("general-assistant"), settings_.defaultPermissionPolicyState(),
                     permissionPolicyService_, toolExecutionGateway_, skillProfileService_,
                     settings_.selectedSkillProfile(), workspaceService_,
                     settings_.selectedWorkspaceId())
        .diagnostics;
}

QString DesktopShellViewModel::controlledTaskActiveSummary() const {
    const auto* service = controller_.controlledTasks();
    if (!service)
        return QStringLiteral("No controlled agent task is active.");
    for (const auto& task : service->tasks())
        if (task.state == core::ControlledTaskState::Running ||
            task.state == core::ControlledTaskState::WaitingApproval)
            return core::controlledAgentTaskSummary(task);
    for (const auto& task : service->tasks())
        if (task.workspaceId == selectedWorkspaceId() &&
            task.state == core::ControlledTaskState::PendingApproval)
            return core::controlledAgentTaskSummary(task);
    return QStringLiteral("No controlled agent task is active.");
}

bool DesktopShellViewModel::controlledTaskSessionActive() const {
    const auto* service = controller_.controlledTasks();
    return service && service->sessionActive();
}

QString DesktopShellViewModel::controlledTaskCurrentStep() const {
    const auto* service = controller_.controlledTasks();
    if (!service)
        return QStringLiteral("No visible step is running.");
    for (const auto& task : service->tasks())
        if ((task.state == core::ControlledTaskState::Running ||
             task.state == core::ControlledTaskState::WaitingApproval) &&
            task.currentStepIndex >= 0 && task.currentStepIndex < task.steps.size())
            return core::controlledAgentStepSummary(task.steps.at(task.currentStepIndex));
    return QStringLiteral("No visible step is running.");
}

QString DesktopShellViewModel::controlledTaskProgressSummary() const {
    const auto* service = controller_.controlledTasks();
    if (!service)
        return QStringLiteral("No running task progress.");
    for (const auto& task : service->tasks()) {
        if (task.state != core::ControlledTaskState::Running &&
            task.state != core::ControlledTaskState::WaitingApproval)
            continue;
        int completed = 0;
        for (const auto& step : task.steps)
            if (step.state == core::ControlledTaskState::Completed ||
                step.state == core::ControlledTaskState::Cancelled ||
                step.state == core::ControlledTaskState::Failed)
                ++completed;
        return QStringLiteral("%1 of %2 step(s) resolved. Remaining: %3.")
            .arg(completed)
            .arg(task.steps.size())
            .arg(std::max(0, static_cast<int>(task.steps.size()) - completed));
    }
    return QStringLiteral("No running task progress.");
}

QStringList DesktopShellViewModel::controlledTaskPlanSteps() const {
    const auto* service = controller_.controlledTasks();
    if (!service)
        return {};
    for (auto it = service->tasks().crbegin(); it != service->tasks().crend(); ++it) {
        if (it->workspaceId != selectedWorkspaceId())
            continue;
        QStringList steps;
        for (const auto& step : it->steps)
            steps.append(core::controlledAgentStepSummary(step));
        return steps;
    }
    return {};
}

QStringList DesktopShellViewModel::controlledTaskQueueSummaries() const {
    const auto* service = controller_.controlledTasks();
    return service ? service->presentation().queueSummaries(service->tasks(), selectedWorkspaceId())
                   : QStringList{};
}

QStringList DesktopShellViewModel::controlledTaskTimelineSummaries() const {
    const auto* service = controller_.controlledTasks();
    return service ? service->presentation().timelineSummaries(
                         service->tasks(), selectedWorkspaceId(), QStringLiteral("all"))
                   : QStringList{};
}

QStringList DesktopShellViewModel::controlledTaskExplainabilitySummaries() const {
    const auto* service = controller_.controlledTasks();
    if (!service)
        return {};
    for (auto it = service->tasks().crbegin(); it != service->tasks().crend(); ++it)
        if (it->workspaceId == selectedWorkspaceId())
            return service->presentation().explainabilitySummaries(*it);
    return {QStringLiteral("No controlled task explainability record yet.")};
}

QStringList DesktopShellViewModel::controlledTaskPermissionSummaries() const {
    const auto* service = controller_.controlledTasks();
    return service ? service->presentation().permissionSummaries(service->permissions(),
                                                                 selectedWorkspaceId())
                   : QStringList{};
}

QStringList DesktopShellViewModel::controlledTaskNotificationCategories() const {
    const auto* service = controller_.controlledTasks();
    return service ? service->presentation().notificationCategories() : QStringList{};
}

QStringList DesktopShellViewModel::controlledTaskExportSummaries() const {
    const auto* service = controller_.controlledTasks();
    return service ? service->presentation().exportCenterSummaries() : QStringList{};
}

QString DesktopShellViewModel::controlledTaskDiagnosticsSummary() const {
    const auto* service = controller_.controlledTasks();
    if (!service)
        return {};
    const auto diagnostics = service->presentation().diagnostics(service->tasks());
    return QStringLiteral("%1 / Last completed: %2 / Approvals: %3 / Failures: %4")
        .arg(diagnostics.activeTask, diagnostics.lastCompletedTask, diagnostics.approvalStatistics,
             diagnostics.failureStatistics);
}

QStringList DesktopShellViewModel::controlledTaskSafetyGuarantees() const {
    const auto* service = controller_.controlledTasks();
    return service ? service->presentation().safetyGuarantees() : QStringList{};
}

QString DesktopShellViewModel::planControlledAgentTask(const QString& goal) {
    auto* service = controller_.controlledTasks();
    if (!service)
        return {};
    QStringList resources;
    for (const auto& attachment : attachments_)
        if (attachment.workspaceId == selectedWorkspaceId())
            resources.append(attachment.fileName);
    return service->createTask(goal, selectedWorkspaceId(), resources);
}

bool DesktopShellViewModel::modifyControlledAgentPlan(const QString& taskId,
                                                      const QStringList& steps) {
    auto* service = controller_.controlledTasks();
    return service && service->editPlan(taskId, steps);
}

bool DesktopShellViewModel::approveControlledAgentTask(const QString& taskId,
                                                       const QString& choice) {
    auto* service = controller_.controlledTasks();
    return service && service->approveTask(taskId, choice);
}

bool DesktopShellViewModel::denyControlledAgentTask(const QString& taskId) {
    auto* service = controller_.controlledTasks();
    return service && service->denyTask(taskId);
}

bool DesktopShellViewModel::startControlledAgentTask(const QString& taskId) {
    auto* service = controller_.controlledTasks();
    return service && service->startTask(taskId);
}

bool DesktopShellViewModel::executeControlledAgentStep(const QString& taskId) {
    auto* service = controller_.controlledTasks();
    return service && service->approveTask(taskId, QStringLiteral("approve"));
}

bool DesktopShellViewModel::skipControlledAgentStep(const QString& taskId) {
    auto* service = controller_.controlledTasks();
    return service && service->skipTask(taskId);
}

bool DesktopShellViewModel::retryControlledAgentStep(const QString& taskId) {
    auto* service = controller_.controlledTasks();
    return service && service->retryTask(taskId);
}

bool DesktopShellViewModel::cancelControlledAgentTask(const QString& taskId) {
    auto* service = controller_.controlledTasks();
    return service && service->cancelTask(taskId);
}

bool DesktopShellViewModel::reorderControlledAgentTask(const QString& taskId, int newIndex) {
    auto* service = controller_.controlledTasks();
    return service && service->reorderTask(taskId, newIndex);
}

bool DesktopShellViewModel::setControlledToolPermission(const QString& category,
                                                        const QString& choice) {
    auto* service = controller_.controlledTasks();
    return service &&
           service->setPermission(selectedWorkspaceId(), category.trimmed(), choice.trimmed());
}

bool DesktopShellViewModel::exportControlledAgentTask(const QString& taskId,
                                                      const QString& format) {
    const auto* service = controller_.controlledTasks();
    if (!service)
        return false;
    const auto task = service->task(taskId);
    if (task.id.isEmpty()) {
        return false;
    }
    QDir directory(QDir(appDataPath()).filePath(QStringLiteral("exports")));
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        return false;
    }
    const auto normalized = format.trimmed().toLower();
    const auto extension =
        normalized == QStringLiteral("json")
            ? QStringLiteral("json")
            : (normalized == QStringLiteral("txt")
                   ? QStringLiteral("txt")
                   : (normalized == QStringLiteral("pdf") ? QStringLiteral("pdf")
                                                          : QStringLiteral("md")));
    const auto path =
        directory.filePath(QStringLiteral("%1-controlled-task.%2").arg(task.id, extension));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(service->presentation().exportTaskReport(task, format));
    const auto committed = file.commit();
    emit controlledAgentTasksChanged();
    return committed;
}

bool DesktopShellViewModel::sendMessage(const QString& message) {
    QStringList paths;
    for (const auto& attachment : attachments_) {
        const auto path = attachmentPaths_.value(attachment.id);
        if (path.isEmpty()) { attachmentError_ = tr("Reattach this file before sending."); emit attachmentChanged(); return false; }
        paths.append(path);
    }
    bool accepted = false;
    if (auto* remote = controller_.remote()) {
        QVariantList values;
        for (const auto& path : paths) values.append(path);
        accepted = paths.isEmpty() ? (currentModeName() == "Agent" ? runAgentRequest(message) : controller_.sendMessage(message))
            : remote->dispatch(currentModeName() == "Agent" ? "runAgentRequestWithAttachments" : "sendMessageWithAttachments", {message, QVariant(values)}).toBool();
    } else {
        const auto loaded = core::loadChatAttachments(paths);
        if (!loaded.ok()) { attachmentError_ = loaded.error; emit attachmentChanged(); return false; }
        accepted = currentModeName() == "Agent" ? controller_.local()->runAgentRequestWithAttachments(message, loaded.attachments)
            : controller_.local()->sendMessageWithAttachments(message, loaded.attachments);
    }
    if (accepted && controller_.isRemote() && !paths.isEmpty()) attachmentSendPending_ = true;
    else if (accepted) clearAttachments();
    return accepted;
}

bool DesktopShellViewModel::regenerateChatResponse(int userMessageId) {
    return controller_.regenerateChatResponse(userMessageId);
}

bool DesktopShellViewModel::regenerateLatestChatResponse() {
    int latestUserId = 0;
    for (const auto& message : controller_.chatHistory())
        if (message.role == core::ChatRole::User)
            latestUserId = message.id;
    return latestUserId > 0 && controller_.regenerateChatResponse(latestUserId);
}

bool DesktopShellViewModel::retryChatResponse(int assistantMessageId) {
    return controller_.retryChatResponse(assistantMessageId);
}

QString DesktopShellViewModel::editAndResendChatMessage(int userMessageId, const QString& text) {
    return controller_.editAndResendChatMessage(userMessageId, text);
}

QVariantList DesktopShellViewModel::searchChats(const QString& query) const {
    return controller_.searchChats(query);
}

bool DesktopShellViewModel::cancelLocalInference() {
    return controller_.chatGenerationActive() ? controller_.stopChatGeneration()
                                              : controller_.cancelLocalInference();
}

bool DesktopShellViewModel::runLocalInference(const QString& prompt, const QString& model) {
    return controller_.runLocalInference(prompt, model);
}

bool DesktopShellViewModel::requestConversationSummaryGeneration() {
    return controller_.requestConversationSummaryGeneration();
}

bool DesktopShellViewModel::searchConversation(const QString& query) {
    return controller_.searchConversation(query);
}

void DesktopShellViewModel::clearConversationSearch() {
    controller_.clearConversationSearch();
}

bool DesktopShellViewModel::exportTranscript(const QString& format) {
    return controller_.exportTranscript(format);
}

void DesktopShellViewModel::openUpdateModal() {
    emit requestOpenUpdateModal();
}

bool DesktopShellViewModel::checkForUpdates() {
    settings_.setUpdateWorkflowState(QStringLiteral("Checking for updates..."));
    emit nativeExperienceChanged();

    if (!QSslSocket::supportsSsl()) {
        settings_.setUpdateWorkflowState(
            QStringLiteral("Update check failed: SSL/TLS is not available on this system"));
        emit nativeExperienceChanged();
        emit updateCheckCompleted(false, QString(), QString(), QString(), 0);
        return false;
    }

    if (!networkManager_) {
        networkManager_ = new QNetworkAccessManager(this);
    }

    const QString userAgent = QStringLiteral("Sentinel/%1").arg(core::AppMetadata::version());
    QNetworkRequest request(QUrl(settings_.updateCheckUrl()));
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
    request.setRawHeader("Accept", "application/vnd.github.v3+json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(10000);

    QNetworkReply* reply = networkManager_->get(request);
    connect(reply, &QNetworkReply::sslErrors, reply, [reply](const QList<QSslError>& errors) {
        for (const auto& err : errors)
            qWarning() << "SSL error:" << err.errorString();
    });

    QTimer::singleShot(10000, reply, [reply]() {
        if (reply && reply->isRunning()) {
            qWarning() << "Update check timed out after 10s, aborting network reply.";
            reply->abort();
        }
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            settings_.setUpdateWorkflowState(
                QStringLiteral("Update check failed: %1").arg(reply->errorString()));
            emit nativeExperienceChanged();
            emit updateCheckCompleted(false, QString(), QString(), QString(), 0);
            return;
        }

        const QByteArray data = reply->readAll();
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            settings_.setUpdateWorkflowState(
                QStringLiteral("Update check failed: invalid response"));
            emit nativeExperienceChanged();
            emit updateCheckCompleted(false, QString(), QString(), QString(), 0);
            return;
        }

        QJsonObject root;
        if (doc.isArray()) {
            const QJsonArray arr = doc.array();
            if (arr.isEmpty()) {
                settings_.setUpdateWorkflowState(
                    QStringLiteral("Update check failed: empty response"));
                emit nativeExperienceChanged();
                emit updateCheckCompleted(false, QString(), QString(), QString(), 0);
                return;
            }
            root = arr.first().toObject();
        } else if (doc.isObject()) {
            root = doc.object();
        } else {
            settings_.setUpdateWorkflowState(
                QStringLiteral("Update check failed: invalid response"));
            emit nativeExperienceChanged();
            emit updateCheckCompleted(false, QString(), QString(), QString(), 0);
            return;
        }
        const QString latestTag = root.value(QStringLiteral("tag_name")).toString();
        const QString htmlUrl = root.value(QStringLiteral("html_url")).toString();
        const QString bodyText = root.value(QStringLiteral("body")).toString();

        if (latestTag.isEmpty()) {
            const QString apiMsg = root.value(QStringLiteral("message")).toString();
            const QString errState =
                apiMsg.isEmpty() ? QStringLiteral("Update check failed: release info not found")
                                 : QStringLiteral("Update check failed: %1").arg(apiMsg);
            settings_.setUpdateWorkflowState(errState);
            emit nativeExperienceChanged();
            emit updateCheckCompleted(false, QString(), QString(), QString(), 0);
            return;
        }

        // Find the best asset for the current platform
        const QString suffix = platformAssetSuffix();
        QString assetName;
        qint64 assetSize = 0;
        QString assetDownloadUrl;

        const QJsonArray assets = root.value(QStringLiteral("assets")).toArray();
        for (const QJsonValue& val : assets) {
            const QJsonObject asset = val.toObject();
            const QString name = asset.value(QStringLiteral("name")).toString();
            if (!suffix.isEmpty() && name.endsWith(suffix, Qt::CaseInsensitive)) {
                assetName = name;
                assetSize = static_cast<qint64>(asset.value(QStringLiteral("size")).toDouble());
                assetDownloadUrl = asset.value(QStringLiteral("browser_download_url")).toString();
                break;
            }
        }

        // Fallback: use first asset if no platform match or no suffix
        if (assetDownloadUrl.isEmpty() && !assets.isEmpty()) {
            const QJsonObject first = assets.first().toObject();
            assetName = first.value(QStringLiteral("name")).toString();
            assetSize = static_cast<qint64>(first.value(QStringLiteral("size")).toDouble());
            assetDownloadUrl = first.value(QStringLiteral("browser_download_url")).toString();
        }

        lastReleaseUrl_ = htmlUrl;
        lastAssetUrl_ = assetDownloadUrl;
        lastAssetName_ = assetName;
        lastAssetSize_ = assetSize;

        const QString currentVersion = core::AppMetadata::version();
        const bool available = isNewerVersion(currentVersion, latestTag);

        settings_.setUpdateWorkflowState(
            available ? QStringLiteral("Available: %1").arg(latestTag)
                      : QStringLiteral("Up to date: %1").arg(currentVersion));

        if (available) {
            const QString title = QStringLiteral("Update Available");
            const QString body = QStringLiteral("New version %1 is available! Details: %2")
                                     .arg(latestTag)
                                     .arg(bodyText.left(120));
            QString updatedJson;
            if (!updateNotification(
                    settings_.notificationCenterJson(), QStringLiteral("updates-manual"),
                    [title, body](QJsonObject& item) {
                        item.insert(QStringLiteral("read"), false);
                        item.insert(QStringLiteral("archived"), false);
                        item.insert(QStringLiteral("title"), title);
                        item.insert(QStringLiteral("body"), body);
                    },
                    &updatedJson)) {
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                QJsonObject item;
                item.insert(QStringLiteral("id"), QStringLiteral("updates-manual"));
                item.insert(QStringLiteral("category"), QStringLiteral("Updates"));
                item.insert(QStringLiteral("title"), title);
                item.insert(QStringLiteral("body"), body);
                item.insert(QStringLiteral("priority"), QStringLiteral("Normal"));
                item.insert(QStringLiteral("timestamp"), now);
                item.insert(QStringLiteral("pinned"), false);
                item.insert(QStringLiteral("archived"), false);
                item.insert(QStringLiteral("read"), false);
                auto notifications = notificationsFromJson(settings_.notificationCenterJson());
                notifications.prepend(item);
                settings_.setNotificationCenterJson(notificationsToJson(notifications, settings_.notificationCenterJson()));
            } else {
                settings_.setNotificationCenterJson(updatedJson);
            }
        } else {
            // Reset stale "Update Available" notification when up-to-date
            QString updatedJson;
            if (updateNotification(
                    settings_.notificationCenterJson(), QStringLiteral("updates-manual"),
                    [](QJsonObject& item) {
                        if (item.value(QStringLiteral("title")).toString() ==
                            QStringLiteral("Update Available")) {
                            item.insert(QStringLiteral("title"),
                                        QStringLiteral("Manual updates only"));
                            item.insert(QStringLiteral("body"),
                                        QStringLiteral("Update checks are performed on demand "
                                                       "or per schedule. No automatic updates."));
                            item.insert(QStringLiteral("read"), true);
                            item.insert(QStringLiteral("archived"), false);
                        }
                    },
                    &updatedJson)) {
                settings_.setNotificationCenterJson(updatedJson);
            }
        }

        emit nativeExperienceChanged();
        emit updateCheckCompleted(available, latestTag, bodyText, htmlUrl, assetSize);
    });

    return true;
}

bool DesktopShellViewModel::startDownload(const QString& assetUrl) {
    if (downloadReply_) {
        return false;
    }

    const QString downloadsDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QDir().mkpath(downloadsDir);

    const QString fileName =
        lastAssetName_.isEmpty() ? QStringLiteral("Sentinel-Update") : lastAssetName_;

    downloadPath_ = QDir(downloadsDir).filePath(fileName);

    if (downloadFile_) {
        downloadFile_->close();
        downloadFile_->remove();
        delete downloadFile_;
        downloadFile_ = nullptr;
    }

    downloadFile_ = new QFile(downloadPath_, this);
    if (!downloadFile_->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete downloadFile_;
        downloadFile_ = nullptr;
        settings_.setUpdateWorkflowState(QStringLiteral("Download failed: cannot write file"));
        emit nativeExperienceChanged();
        return false;
    }

    if (!networkManager_) {
        networkManager_ = new QNetworkAccessManager(this);
    }

    lastBytesReceived_ = 0;
    downloadElapsed_.start();

    const QUrl reqUrl(assetUrl);
    QNetworkRequest request(reqUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Sentinel/%1").arg(core::AppMetadata::version()));
    request.setTransferTimeout(30000);

    downloadReply_ = networkManager_->get(request);
    connect(downloadReply_, &QNetworkReply::sslErrors, downloadReply_, [this]() {
        if (downloadReply_)
            downloadReply_->ignoreSslErrors();
    });

    connect(downloadReply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 bytesReceived, qint64 bytesTotal) {
                if (bytesTotal <= 0)
                    return;

                const qint64 elapsed = downloadElapsed_.elapsed();
                const double speed =
                    elapsed > 0 ? (bytesReceived - lastBytesReceived_) / (elapsed / 1000.0) : 0.0;
                if (elapsed >= 500) {
                    lastBytesReceived_ = bytesReceived;
                    downloadElapsed_.start();
                }

                emit updateDownloadProgressChanged(bytesReceived, bytesTotal, speed);
            });

    connect(downloadReply_, &QNetworkReply::readyRead, this, [this]() {
        if (downloadFile_ && downloadReply_) {
            downloadFile_->write(downloadReply_->readAll());
        }
    });

    connect(downloadReply_, &QNetworkReply::finished, this, [this]() {
        if (downloadReply_) {
            downloadReply_->deleteLater();
            downloadReply_ = nullptr;
        }

        if (downloadFile_) {
            downloadFile_->close();
            downloadFile_->deleteLater();
            downloadFile_ = nullptr;
        }

        const bool success =
            QFileInfo::exists(downloadPath_) && QFileInfo(downloadPath_).size() > 0;
        if (success) {
            settings_.setUpdateWorkflowState(QStringLiteral("Downloaded: %1").arg(downloadPath_));
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(downloadPath_).absolutePath()));
        } else {
            settings_.setUpdateWorkflowState(QStringLiteral("Download failed"));
            if (QFileInfo::exists(downloadPath_)) {
                QFile::remove(downloadPath_);
            }
        }

        emit nativeExperienceChanged();
        emit updateDownloadFinished(success, downloadPath_);
    });

    settings_.setUpdateWorkflowState(QStringLiteral("Downloading update..."));
    emit nativeExperienceChanged();
    return true;
}

void DesktopShellViewModel::cancelDownload() {
    if (downloadReply_) {
        downloadReply_->abort();
        downloadReply_->deleteLater();
        downloadReply_ = nullptr;
    }
    if (downloadFile_) {
        downloadFile_->close();
        downloadFile_->remove();
        downloadFile_->deleteLater();
        downloadFile_ = nullptr;
    }
    if (!downloadPath_.isEmpty() && QFileInfo::exists(downloadPath_)) {
        QFile::remove(downloadPath_);
    }
    settings_.setUpdateWorkflowState(QStringLiteral("Download cancelled"));
    emit nativeExperienceChanged();
}

bool DesktopShellViewModel::confirmUpdateDownload() {
    QString urlStr = lastAssetUrl_.isEmpty() ? lastReleaseUrl_ : lastAssetUrl_;
    if (urlStr.isEmpty()) {
        urlStr = QStringLiteral("https://github.com/Sopwit/Sentinel/releases");
    }
    const bool opened = QDesktopServices::openUrl(QUrl(urlStr));
    if (opened) {
        settings_.setUpdateWorkflowState(QStringLiteral("Redirected to download page"));
    } else {
        settings_.setUpdateWorkflowState(QStringLiteral("Failed to open browser"));
    }
    emit nativeExperienceChanged();
    return opened;
}

QString DesktopShellViewModel::globalErrorMessage() const {
    return globalErrorMessage_;
}

QString DesktopShellViewModel::globalErrorSeverity() const {
    return globalErrorSeverity_;
}

bool DesktopShellViewModel::globalErrorVisible() const {
    return globalErrorVisible_;
}

QString DesktopShellViewModel::globalErrorActionLabel() const {
    return globalErrorActionLabel_;
}

void DesktopShellViewModel::setGlobalError(const QString& message, const QString& severity,
                                           const QString& actionLabel) {
    applyGlobalError(message, severity, actionLabel, DesktopErrorCategory::None);
}

void DesktopShellViewModel::applyGlobalError(const QString& message, const QString& severity,
                                             const QString& actionLabel,
                                             DesktopErrorCategory category) {
    if (message.isEmpty()) {
        clearGlobalError();
        return;
    }
    // Deduplicate repeated equivalent failures so re-projection never stacks banners.
    if (globalErrorVisible_ && globalErrorMessage_ == message && globalErrorSeverity_ == severity &&
        globalErrorActionLabel_ == actionLabel && globalErrorCategory_ == category) {
        return;
    }
    globalErrorMessage_ = message;
    globalErrorSeverity_ = severity;
    globalErrorActionLabel_ = actionLabel;
    globalErrorVisible_ = true;
    globalErrorCategory_ = category;
    emit globalErrorChanged();
}

void DesktopShellViewModel::clearGlobalError() {
    if (!globalErrorVisible_ && globalErrorMessage_.isEmpty()) {
        globalErrorCategory_ = DesktopErrorCategory::None;
        return;
    }
    globalErrorVisible_ = false;
    globalErrorMessage_.clear();
    globalErrorActionLabel_.clear();
    globalErrorCategory_ = DesktopErrorCategory::None;
    emit globalErrorChanged();
}

void DesktopShellViewModel::dismissGlobalError() {
    clearGlobalError();
}

void DesktopShellViewModel::triggerGlobalErrorAction() {
    switch (globalErrorCategory_) {
    case DesktopErrorCategory::DaemonUnavailable:
    case DesktopErrorCategory::DaemonDisconnected:
        // Reconnect only: never replay a mutation. Acceptance may be uncertain after a
        // timeout/disconnect, so the transport re-handshakes and re-projects instead.
        if (auto* runtime = controller_.remote())
            runtime->transport().connectToDaemon();
        break;
    default:
        break;
    }
}

void DesktopShellViewModel::reportStartupRecovery(const QString& code) {
    if (code.isEmpty())
        return;
    // The code is a product identifier from RecoveryService; raw detail is never shown.
    applyGlobalError(
        tr("Sentinel recovered from a local settings problem. Review your preferences if "
           "anything looks incorrect."),
        QStringLiteral("Warning"), QString(), DesktopErrorCategory::StartupRecovery);
}

void DesktopShellViewModel::setDaemonAvailabilityGracePeriodMs(int milliseconds) {
    if (daemonGraceTimer_)
        daemonGraceTimer_->setInterval(qMax(0, milliseconds));
}

void DesktopShellViewModel::showDaemonUnavailableBanner() {
    if (daemonUnavailableShown_)
        return;
    daemonUnavailableShown_ = true;
    applyGlobalError(
        tr("The Sentinel daemon is unavailable. Some features stay disabled until it reconnects."),
        QStringLiteral("Warning"), tr("Retry"), DesktopErrorCategory::DaemonUnavailable);
}

void DesktopShellViewModel::refreshDaemonErrorProjection() {
    auto* runtime = controller_.remote();
    if (!runtime)
        return;
    const auto state = runtime->transport().connectionState();
    const auto category = desktopErrorCategoryForDaemonState(state);

    // A serving connection, or a detected loss after one, closes any pending cold-start window.
    // A transient Connecting retry deliberately keeps it open so a slow daemon is not reported.
    if (state == DaemonClient::ConnectionState::Connected ||
        state == DaemonClient::ConnectionState::Reconnecting) {
        daemonUnavailableShown_ = false;
        if (daemonGraceTimer_)
            daemonGraceTimer_->stop();
    }

    if (!category)
        return; // Connecting / Disconnected / VersionMismatch / ShuttingDown: leave as-is.

    switch (*category) {
    case DesktopErrorCategory::DaemonUnavailable:
        // Cold start: the daemon may simply not be up yet, so defer briefly instead of
        // flashing a false alarm. A daemon that stays absent is still surfaced.
        if (daemonGraceTimer_ && daemonGraceTimer_->interval() > 0) {
            if (!daemonGraceTimer_->isActive() && !daemonUnavailableShown_)
                daemonGraceTimer_->start();
        } else {
            showDaemonUnavailableBanner();
        }
        break;
    case DesktopErrorCategory::DaemonDisconnected:
        // The daemon was reachable and dropped: report immediately, do not defer.
        applyGlobalError(tr("Lost connection to the Sentinel daemon. Reconnecting…"),
                         QStringLiteral("Error"), tr("Reconnect"),
                         DesktopErrorCategory::DaemonDisconnected);
        break;
    case DesktopErrorCategory::None:
        // Connection restored: only clear a banner this projection owns.
        if (isDaemonErrorCategory(globalErrorCategory_))
            clearGlobalError();
        break;
    case DesktopErrorCategory::StartupRecovery:
    default:
        break;
    }
}

void DesktopShellViewModel::relaunchApplication() {
    QProcess::startDetached(QCoreApplication::applicationFilePath(), QCoreApplication::arguments());
    QCoreApplication::quit();
}

void DesktopShellViewModel::addNotification(const QString& category, const QString& title,
                                            const QString& body) {
    addNotificationWithPriority(category, title, body, QStringLiteral("Normal"));
}

void DesktopShellViewModel::addNotificationWithPriority(const QString& category,
                                                        const QString& title, const QString& body,
                                                        const QString& priority) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 cooldownMs = 8000;

    const QString json = settings_.notificationCenterJson();
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    QJsonObject root = doc.object();
    QJsonArray arr = root.value(QStringLiteral("notifications")).toArray();

    // Coalesce: if an unread notification with the same category and title exists,
    // update its body and timestamp instead of creating a new entry
    for (qsizetype i = 0; i < arr.size(); ++i) {
        auto item = arr.at(i).toObject();
        if (item.value(QStringLiteral("category")).toString() == category &&
            item.value(QStringLiteral("title")).toString() == title &&
            !item.value(QStringLiteral("read")).toBool()) {
            int sameCount = 1;
            for (qsizetype j = 0; j < arr.size(); ++j) {
                if (i != j) {
                    auto other = arr.at(j).toObject();
                    if (other.value(QStringLiteral("category")).toString() == category &&
                        other.value(QStringLiteral("title")).toString() == title &&
                        !other.value(QStringLiteral("read")).toBool()) {
                        ++sameCount;
                    }
                }
            }
            const QString prefix =
                sameCount > 1 ? QStringLiteral("[%1x] ").arg(sameCount) : QString();
            item.insert(QStringLiteral("body"), prefix + body);
            item.insert(QStringLiteral("timestamp"), now);
            arr.replace(i, item);
            root.insert(QStringLiteral("notifications"), arr);
            settings_.setNotificationCenterJson(
                QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
            emit nativeExperienceChanged();
            return;
        }
    }

    // Rate limit: skip new notification creation if same category was recently notified
    auto cooldownIt = notificationCooldowns_.find(category);
    if (cooldownIt != notificationCooldowns_.end() && (now - cooldownIt.value()) < cooldownMs) {
        return;
    }
    notificationCooldowns_[category] = now;

    QJsonObject item;
    const QString newId = QStringLiteral("notif-%1-%2").arg(now).arg(qHash(title) % 10000);
    item.insert(QStringLiteral("id"), newId);
    item.insert(QStringLiteral("category"), category);
    item.insert(QStringLiteral("title"), title);
    item.insert(QStringLiteral("body"), body);
    item.insert(QStringLiteral("priority"), priority);
    item.insert(QStringLiteral("timestamp"), now);
    item.insert(QStringLiteral("pinned"), false);
    item.insert(QStringLiteral("archived"), false);
    item.insert(QStringLiteral("read"), false);

    arr.prepend(item);
    root.insert(QStringLiteral("notifications"), arr);

    settings_.setNotificationCenterJson(
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    emit nativeExperienceChanged();
}

bool DesktopShellViewModel::dndEnabled() const {
    return QJsonDocument::fromJson(settings_.notificationCenterJson().toUtf8()).object().value(QStringLiteral("doNotDisturb")).toBool();
}

void DesktopShellViewModel::setDndEnabled(bool enabled) {
    if (enabled == dndEnabled()) return;
    auto root = QJsonDocument::fromJson(settings_.notificationCenterJson().toUtf8()).object();
    root.insert(QStringLiteral("doNotDisturb"), enabled);
    settings_.setNotificationCenterJson(QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    emit dndEnabledChanged();
}

bool DesktopShellViewModel::snoozeNotification(const QString& notificationId, int minutes) {
    const qint64 snoozeUntil =
        QDateTime::currentMSecsSinceEpoch() + static_cast<qint64>(minutes) * 60000;
    QString updatedJson;
    if (!updateNotification(
            settings_.notificationCenterJson(), notificationId,
            [snoozeUntil](QJsonObject& item) {
                item.insert(QStringLiteral("snoozed"), true);
                item.insert(QStringLiteral("snoozeUntil"), snoozeUntil);
            },
            &updatedJson)) {
        return false;
    }
    settings_.setNotificationCenterJson(updatedJson);
    emit nativeExperienceChanged();
    return true;
}

bool DesktopShellViewModel::unsnoozeNotification(const QString& notificationId) {
    QString updatedJson;
    if (!updateNotification(
            settings_.notificationCenterJson(), notificationId,
            [](QJsonObject& item) {
                item.insert(QStringLiteral("snoozed"), false);
                item.remove(QStringLiteral("snoozeUntil"));
            },
            &updatedJson)) {
        return false;
    }
    settings_.setNotificationCenterJson(updatedJson);
    emit nativeExperienceChanged();
    return true;
}

bool DesktopShellViewModel::shouldShowNotification(const QVariantMap& item) const {
    const auto category = item.value(QStringLiteral("category")).toString();
    const auto priority = item.value(QStringLiteral("priority"), QStringLiteral("Normal")).toString();
    const auto policy = settings_.notificationPolicy();
    if (dndEnabled() || isChannelMuted(category) || item.value(QStringLiteral("snoozed")).toBool() ||
        policy == QLatin1String("Disabled") || policy == QLatin1String("None")) return false;
    if (policy == QLatin1String("Important Only"))
        return priority == QLatin1String("High") || priority == QLatin1String("Critical");
    if (policy != QLatin1String("Custom")) return true;
    if (category == QLatin1String("Agent")) return settings_.notifyAgentResponses();
    if (category == QLatin1String("Updates")) return settings_.notifySystemUpdates();
    if (category == QLatin1String("Models")) {
        const auto title = item.value(QStringLiteral("title")).toString();
        return title.contains(QLatin1String("Removed")) || title.contains(QLatin1String("Deleted"))
            ? settings_.notifyModelRemovals() : settings_.notifyModelDownloads();
    }
    return true;
}

bool DesktopShellViewModel::isChannelMuted(const QString& category) const {
    return mutedChannelNames().contains(category);
}

void DesktopShellViewModel::setChannelMuted(const QString& category, bool muted) {
    auto channels = mutedChannelNames();
    if (muted) {
        if (!channels.contains(category))
            channels.append(category);
    } else {
        channels.removeAll(category);
    }
    QJsonArray arr;
    for (const auto& ch : channels) {
        arr.append(ch);
    }
    auto doc = QJsonDocument::fromJson(settings_.notificationCenterJson().toUtf8());
    auto root = doc.object();
    root.insert(QStringLiteral("mutedChannels"), arr);
    settings_.setNotificationCenterJson(
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    emit nativeExperienceChanged();
}

QStringList DesktopShellViewModel::mutedChannelNames() const {
    const auto json = settings_.notificationCenterJson();
    const auto doc = QJsonDocument::fromJson(json.toUtf8());
    const auto arr = doc.object().value(QStringLiteral("mutedChannels")).toArray();
    QStringList result;
    for (const auto& v : arr) {
        result.append(v.toString());
    }
    return result;
}

QVariantMap DesktopShellViewModel::cursorScreenGeometry() {
    const auto cursorPos = QCursor::pos();
    const auto screens = QGuiApplication::screens();
    for (const auto* screen : screens) {
        if (screen && screen->geometry().contains(cursorPos)) {
            return {
                {QStringLiteral("x"), screen->availableGeometry().x()},
                {QStringLiteral("y"), screen->availableGeometry().y()},
                {QStringLiteral("width"), screen->availableGeometry().width()},
                {QStringLiteral("height"), screen->availableGeometry().height()},
                {QStringLiteral("screenWidth"), screen->geometry().width()},
                {QStringLiteral("screenHeight"), screen->geometry().height()},
            };
        }
    }
    const auto* primary = QGuiApplication::primaryScreen();
    if (primary) {
        return {
            {QStringLiteral("x"), primary->availableGeometry().x()},
            {QStringLiteral("y"), primary->availableGeometry().y()},
            {QStringLiteral("width"), primary->availableGeometry().width()},
            {QStringLiteral("height"), primary->availableGeometry().height()},
            {QStringLiteral("screenWidth"), primary->geometry().width()},
            {QStringLiteral("screenHeight"), primary->geometry().height()},
        };
    }
    return {};
}

void DesktopShellViewModel::replayOnboarding() {
    settings_.setOnboardingComplete(false);
}

void DesktopShellViewModel::seedRecoveryDraft(const QString& text) {
    settings_.setRecoveryDraftText(text);
}

bool DesktopShellViewModel::pinNotification(const QString& notificationId) {
    QString updatedJson;
    if (!updateNotification(
            settings_.notificationCenterJson(), notificationId,
            [](QJsonObject& item) {
                item.insert(QStringLiteral("pinned"),
                            !item.value(QStringLiteral("pinned")).toBool());
            },
            &updatedJson)) {
        return false;
    }
    settings_.setNotificationCenterJson(updatedJson);
    return true;
}

bool DesktopShellViewModel::archiveNotification(const QString& notificationId) {
    QString updatedJson;
    if (!updateNotification(
            settings_.notificationCenterJson(), notificationId,
            [](QJsonObject& item) {
                item.insert(QStringLiteral("archived"), true);
                item.insert(QStringLiteral("read"), true);
            },
            &updatedJson)) {
        return false;
    }
    settings_.setNotificationCenterJson(updatedJson);
    return true;
}

QString DesktopShellViewModel::latestNotificationId() const {
    const auto notifications = notificationsFromJson(settings_.notificationCenterJson());
    if (notifications.isEmpty())
        return QString();
    const auto first = notifications.at(0).toObject();
    return first.value(QStringLiteral("id")).toString();
}

bool DesktopShellViewModel::markNotificationRead(const QString& notificationId) {
    QString updatedJson;
    if (!updateNotification(
            settings_.notificationCenterJson(), notificationId,
            [](QJsonObject& item) { item.insert(QStringLiteral("read"), true); }, &updatedJson)) {
        return false;
    }
    settings_.setNotificationCenterJson(updatedJson);
    return true;
}

bool DesktopShellViewModel::clearArchivedNotifications() {
    const auto notifications = notificationsFromJson(settings_.notificationCenterJson());
    QJsonArray kept;
    for (const auto& value : notifications) {
        const auto item = value.toObject();
        if (!item.value(QStringLiteral("archived")).toBool()) {
            kept.append(item);
        }
    }
    settings_.setNotificationCenterJson(notificationsToJson(kept, settings_.notificationCenterJson()));
    return true;
}

bool DesktopShellViewModel::removeNotificationById(const QString& notificationId) {
    const auto notifications = notificationsFromJson(settings_.notificationCenterJson());
    QJsonArray kept;
    bool found = false;
    for (const auto& value : notifications) {
        const auto item = value.toObject();
        if (item.value(QStringLiteral("id")).toString() == notificationId) {
            found = true;
            continue;
        }
        kept.append(item);
    }
    if (!found)
        return false;
    settings_.setNotificationCenterJson(notificationsToJson(kept, settings_.notificationCenterJson()));
    emit nativeExperienceChanged();
    return true;
}

bool DesktopShellViewModel::markAllNotificationsRead() {
    const auto notifications = notificationsFromJson(settings_.notificationCenterJson());
    QJsonArray updated;
    bool changed = false;
    for (const auto& value : notifications) {
        auto item = value.toObject();
        if (!item.value(QStringLiteral("read")).toBool()) {
            item.insert(QStringLiteral("read"), true);
            changed = true;
        }
        updated.append(item);
    }
    if (!changed)
        return false;
    settings_.setNotificationCenterJson(notificationsToJson(updated, settings_.notificationCenterJson()));
    emit nativeExperienceChanged();
    return true;
}

bool DesktopShellViewModel::prepareExportPreview(const QString& source, const QString& format) {
    const auto normalizedSource =
        source.trimmed().isEmpty() ? QStringLiteral("conversations") : source.trimmed();
    const auto normalizedFormat =
        format.trimmed().isEmpty() ? QStringLiteral("Markdown") : format.trimmed();
    if (normalizedSource == exportPreviewSource_ && normalizedFormat == exportPreviewFormat_) {
        return true;
    }
    exportPreviewSource_ = normalizedSource;
    exportPreviewFormat_ = normalizedFormat;
    emit nativeExperienceChanged();
    return true;
}

bool DesktopShellViewModel::exportDiagnostics(const QString& format) {
    const auto extension = format.trimmed().toLower() == QStringLiteral("json")
                               ? QStringLiteral("json")
                               : QStringLiteral("txt");
    QDir directory(QDir(appDataPath()).filePath(QStringLiteral("exports")));
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        return false;
    }
    const auto outputPath =
        directory.filePath(QStringLiteral("sentinel-diagnostics.%1").arg(extension));
    QSaveFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }

    QByteArray payload;
    if (extension == QStringLiteral("json")) {
        QJsonObject root;
        root.insert(QStringLiteral("version"), core::AppMetadata::version());
        root.insert(QStringLiteral("projectVersion"), core::AppMetadata::projectVersion());
        root.insert(QStringLiteral("buildNumber"), core::AppMetadata::buildNumber());
        root.insert(QStringLiteral("gitCommit"), core::AppMetadata::gitCommit());
        root.insert(QStringLiteral("buildType"), core::AppMetadata::buildType());
        root.insert(QStringLiteral("platform"), core::AppMetadata::platform());
        root.insert(QStringLiteral("architecture"), core::AppMetadata::architecture());
        root.insert(QStringLiteral("qtVersion"), QString::fromLatin1(qVersion()));
        root.insert(QStringLiteral("activeProvider"), controller_.activeRuntimeProviderLabel());
        root.insert(QStringLiteral("activeModel"), controller_.activeRuntimeModelLabel());
        root.insert(QStringLiteral("workspaceCount"), workspaceIds().size());
        root.insert(QStringLiteral("brainEntryCount"), memoryEntryCount());
        root.insert(QStringLiteral("taskStatistics"), controlledTaskDiagnosticsSummary());
        root.insert(QStringLiteral("notificationStatistics"),
                    notificationLifecycleSummaries().join(QStringLiteral(" / ")));
        payload = QJsonDocument(root).toJson(QJsonDocument::Indented);
    } else {
        payload = diagnosticsCenterSummaries().join(QStringLiteral("\n")).toUtf8();
    }
    if (file.write(payload) != payload.size()) {
        return false;
    }
    return file.commit();
}

bool DesktopShellViewModel::requestConversationExport(const QString& format) {
    return exportTranscript(format);
}

QString DesktopShellViewModel::createConversation(const QString& title) {
    const auto conversationId = controller_.createConversation(title);
    if (!controller_.isRemote() && !conversationId.isEmpty()) {
        settings_.setActiveConversationId(conversationId);
    }
    return conversationId;
}

bool DesktopShellViewModel::switchConversation(const QString& conversationId) {
    const auto switched = controller_.switchConversation(conversationId);
    if (switched) {
        settings_.setActiveConversationId(controller_.activeConversationId());
    }
    return switched;
}

bool DesktopShellViewModel::renameConversation(const QString& conversationId,
                                               const QString& title) {
    return controller_.renameConversation(conversationId, title);
}

bool DesktopShellViewModel::pinConversation(const QString& conversationId) {
    return controller_.pinConversation(conversationId);
}

bool DesktopShellViewModel::unpinConversation(const QString& conversationId) {
    return controller_.unpinConversation(conversationId);
}

QString DesktopShellViewModel::duplicateConversation(const QString& conversationId) {
    return controller_.duplicateConversation(conversationId);
}

bool DesktopShellViewModel::archiveConversation(const QString& conversationId) {
    const auto archived = controller_.archiveConversation(conversationId);
    if (archived) {
        settings_.setActiveConversationId(controller_.activeConversationId());
    }
    return archived;
}

bool DesktopShellViewModel::unarchiveConversation(const QString& conversationId) {
    const auto unarchived = controller_.unarchiveConversation(conversationId);
    if (unarchived && conversationId == controller_.activeConversationId()) {
        settings_.setActiveConversationId(controller_.activeConversationId());
    }
    return unarchived;
}

bool DesktopShellViewModel::requestPermanentDeleteConversation(const QString& conversationId) {
    return controller_.requestPermanentDeleteConversation(conversationId);
}

QString DesktopShellViewModel::createMemoryCandidateFromConversationText(const QString& text) {
    return controller_.createMemoryCandidateFromConversationText(text);
}

bool DesktopShellViewModel::approveMemoryCandidate(const QString& candidateId) {
    return controller_.approveMemoryCandidate(candidateId);
}

bool DesktopShellViewModel::rejectMemoryCandidate(const QString& candidateId) {
    return controller_.rejectMemoryCandidate(candidateId);
}

bool DesktopShellViewModel::resetMemoryCandidate(const QString& candidateId) {
    return controller_.resetMemoryCandidate(candidateId);
}

bool DesktopShellViewModel::archiveMemoryCandidate(const QString& candidateId) {
    return controller_.archiveMemoryCandidate(candidateId);
}

bool DesktopShellViewModel::requestMemoryCandidateCommit(const QString& candidateId) {
    return controller_.requestMemoryCandidateCommit(candidateId);
}

bool DesktopShellViewModel::recallLocalMemory(const QString& query) {
    return controller_.recallLocalMemory(query);
}

void DesktopShellViewModel::clearLocalMemoryRecall() {
    controller_.clearLocalMemoryRecall();
}

bool DesktopShellViewModel::runAgentRequest(const QString& request) {
    return controller_.runAgentRequest(request);
}

bool DesktopShellViewModel::clearMemory() {
    return controller_.clearMemory();
}

bool DesktopShellViewModel::clearChat() {
    return controller_.clearChat();
}

void DesktopShellViewModel::setModeByName(const QString& modeName) {
    modeManager_.setModeByName(modeName);
}

void DesktopShellViewModel::remember(const QString& key, const QString& value) {
    controller_.remember(key, value);
}

bool DesktopShellViewModel::notifyModelDownloads() const {
    return settings_.notifyModelDownloads();
}

void DesktopShellViewModel::setNotifyModelDownloads(bool enabled) {
    settings_.setNotifyModelDownloads(enabled);
}

bool DesktopShellViewModel::notifyModelRemovals() const {
    return settings_.notifyModelRemovals();
}

void DesktopShellViewModel::setNotifyModelRemovals(bool enabled) {
    settings_.setNotifyModelRemovals(enabled);
}

bool DesktopShellViewModel::notifyAgentResponses() const {
    return settings_.notifyAgentResponses();
}

void DesktopShellViewModel::setNotifyAgentResponses(bool enabled) {
    settings_.setNotifyAgentResponses(enabled);
}

bool DesktopShellViewModel::notifySystemUpdates() const {
    return settings_.notifySystemUpdates();
}

void DesktopShellViewModel::setNotifySystemUpdates(bool enabled) {
    settings_.setNotifySystemUpdates(enabled);
}

bool DesktopShellViewModel::soundEffectsEnabled() const {
    return settings_.soundEffectsEnabled();
}

void DesktopShellViewModel::setSoundEffectsEnabled(bool enabled) {
    settings_.setSoundEffectsEnabled(enabled);
}

QString DesktopShellViewModel::openAiApiKey() const {
    return {};
}

void DesktopShellViewModel::setOpenAiApiKey(const QString& key) {
    if (key.trimmed().isEmpty())
        return;
    if (controller_.isRemote()) {
        setProductProviderCredential(QStringLiteral("openai"), key);
        return;
    }
    settings_.setOpenAiApiKey(key);
}

QString DesktopShellViewModel::claudeApiKey() const {
    return {};
}

void DesktopShellViewModel::setClaudeApiKey(const QString& key) {
    if (key.trimmed().isEmpty())
        return;
    if (controller_.isRemote()) {
        setProductProviderCredential(QStringLiteral("claude"), key);
        return;
    }
    settings_.setClaudeApiKey(key);
}

QString DesktopShellViewModel::geminiApiKey() const {
    return {};
}

void DesktopShellViewModel::setGeminiApiKey(const QString& key) {
    if (key.trimmed().isEmpty())
        return;
    if (controller_.isRemote()) {
        setProductProviderCredential(QStringLiteral("gemini"), key);
        return;
    }
    settings_.setGeminiApiKey(key);
}

QString DesktopShellViewModel::deepseekApiKey() const {
    return {};
}

void DesktopShellViewModel::setDeepseekApiKey(const QString& key) {
    if (key.trimmed().isEmpty())
        return;
    if (controller_.isRemote()) {
        setProductProviderCredential(QStringLiteral("deepseek"), key);
        return;
    }
    settings_.setDeepseekApiKey(key);
}

QString DesktopShellViewModel::groqApiKey() const {
    return {};
}

void DesktopShellViewModel::setGroqApiKey(const QString& key) {
    if (key.trimmed().isEmpty())
        return;
    if (controller_.isRemote()) {
        setProductProviderCredential(QStringLiteral("groq"), key);
        return;
    }
    settings_.setGroqApiKey(key);
}

QString DesktopShellViewModel::mistralApiKey() const {
    return {};
}

void DesktopShellViewModel::setMistralApiKey(const QString& key) {
    if (key.trimmed().isEmpty())
        return;
    if (controller_.isRemote()) {
        setProductProviderCredential(QStringLiteral("mistral"), key);
        return;
    }
    settings_.setMistralApiKey(key);
}

bool DesktopShellViewModel::notificationCenterVisible() const {
    return notificationCenterVisible_;
}

void DesktopShellViewModel::setNotificationCenterVisible(bool visible) {
    if (visible == notificationCenterVisible_)
        return;
    notificationCenterVisible_ = visible;
    emit notificationCenterVisibleChanged();
}

void DesktopShellViewModel::toggleNotificationCenter() {
    setNotificationCenterVisible(!notificationCenterVisible_);
}

QString DesktopShellViewModel::normalizedPageOrDefault(const QString& page) {
    const auto trimmed = page.trimmed();
    const QStringList pages{
        QStringLiteral("Memory"),
        QStringLiteral("Dashboard"),
        QStringLiteral("Agents"),
        QStringLiteral("Settings"),
    };
    return pages.contains(trimmed) ? trimmed : QStringLiteral("Dashboard");
}

} // namespace sentinel::desktop

namespace sentinel::desktop {
bool DesktopShellViewModel::respondToDaemonApproval(bool allow) {
    return controller_.remote() && controller_.remote()->respondToApproval(allow);
}
bool DesktopShellViewModel::cancelDaemonRun() {
    return controller_.remote() && controller_.remote()->cancelRun();
}
QVariantMap DesktopShellViewModel::pendingDaemonApproval() const {
    return controller_.remote() ? controller_.remote()->pendingApproval().toVariantMap()
                                : QVariantMap{};
}
QString DesktopShellViewModel::daemonConnectionStatus() const {
    if (const auto* runtime = controller_.remote()) {
        if (runtime->transport().daemonReachable())
            return runtime->ready() ? tr("Ready — daemon connected")
                                    : tr("Loading authoritative state…");
        return runtime->transport().statusSummary();
    }
    return QStringLiteral("Legacy test fixture");
}
bool DesktopShellViewModel::daemonConnected() const {
    return controller_.remote() && controller_.remote()->transport().daemonReachable();
}
} // namespace sentinel::desktop
