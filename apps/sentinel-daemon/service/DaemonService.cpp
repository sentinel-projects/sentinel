// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "DaemonService.h"
#include "DaemonDesktopSettings.h"
#include <QNetworkProxy>

#include "sentinel/core/app/ApplicationControllerBuilder.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QStandardPaths>

namespace sentinel::daemon {

DaemonService::DaemonService(QObject* parent) : QObject(parent) {
    connect(&m_ipcServer, &DaemonIpcServer::shutdownStarted, this, [this] {
        m_healthTimer.stop();
        if (m_localModelRuntime) {
            m_localModelRuntime->beginShutdown();
        }
    });
    connect(&m_ipcServer, &DaemonIpcServer::shutdownDrained, this,
            [] { QCoreApplication::quit(); });
    connect(&m_healthTimer, &QTimer::timeout, this, &DaemonService::performHealthCheck);
}

DaemonService::~DaemonService() {
    m_healthTimer.stop();
    if (m_resourceMonitor) {
        m_resourceMonitor->stop();
    }
    if (m_controller) {
        m_controller->stopChatGeneration();
        if (m_controller->agentRuntime()) {
            m_controller->agentRuntime()->shutdown();
        }
    }
    m_ipcServer.setController(nullptr);
    m_localModelRuntime.reset();
    m_controller.reset();
    m_settings.reset();
    m_ipcServer.stopServer();
}

bool DaemonService::initialize(const QString& socketPath) {
    qInfo().noquote() << "Initializing Sentinel Headless Daemon Service...";

    // Claim the user endpoint before opening stores or recovering interrupted runs.
    // A losing second daemon must not mutate the active daemon's persistence.
    if (!m_ipcServer.startServer(socketPath)) {
        return false;
    }

    if (!m_pathProvider.isPortable()) {
        m_pathProvider.setProfileDirectory(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    }

    if (!m_profileWriteLease.acquire(m_pathProvider)) {
        qCritical() << "Qt profile is reserved for a Rust cutover.";
        m_ipcServer.stopServer();
        return false;
    }

    m_settings = std::make_unique<sentinel::core::AppSettings>(
        std::make_unique<sentinel::core::DpapiEncryptedSettingsStore>(
            std::make_unique<sentinel::core::JsonSettingsStore>(
                m_pathProvider.settingsFilePath())));

    sentinel::core::ApplicationControllerBuilder builder;
    m_controller = builder.withStandardDefaults(m_pathProvider, *m_settings).build();
    m_controller->attachControlledTaskSettings(*m_settings);
    m_controller->setToolPermissionPolicyState(m_settings->defaultPermissionPolicyState());
    connect(
        m_settings.get(), &core::AppSettings::defaultPermissionPolicyStateChanged, this, [this] {
            m_controller->setToolPermissionPolicyState(m_settings->defaultPermissionPolicyState());
        });
    m_controller->setRoutingModeByName(m_settings->routingModeName());
    connect(m_settings.get(), &core::AppSettings::routingModeNameChanged, this,
            [this] { m_controller->setRoutingModeByName(m_settings->routingModeName()); });
    m_controller->setOllamaEndpoint(m_settings->ollamaEndpoint());
    connect(m_settings.get(), &core::AppSettings::ollamaEndpointChanged, this,
            [this] { m_controller->setOllamaEndpoint(m_settings->ollamaEndpoint()); });
    m_controller->setLmStudioEndpoint(m_settings->lmStudioEndpoint());
    connect(m_settings.get(), &core::AppSettings::lmStudioEndpointChanged, this,
            [this] { m_controller->setLmStudioEndpoint(m_settings->lmStudioEndpoint()); });
    m_controller->setLlamaCppEndpoint(m_settings->llamaCppEndpoint());
    connect(m_settings.get(), &core::AppSettings::llamaCppEndpointChanged, this,
            [this] { m_controller->setLlamaCppEndpoint(m_settings->llamaCppEndpoint()); });
    m_controller->setLocalChatInferenceEnabled(m_settings->localChatInferenceEnabled());
    connect(m_settings.get(), &core::AppSettings::localChatInferenceEnabledChanged, this, [this] {
        m_controller->setLocalChatInferenceEnabled(m_settings->localChatInferenceEnabled());
    });
    m_controller->setLocalInferenceStreamingEnabled(m_settings->localInferenceStreamingEnabled());
    connect(m_settings.get(), &core::AppSettings::localInferenceStreamingEnabledChanged, this,
            [this] {
                m_controller->setLocalInferenceStreamingEnabled(
                    m_settings->localInferenceStreamingEnabled());
            });
    m_controller->setLocalInferenceTimeoutMs(m_settings->localInferenceTimeoutMs());
    connect(m_settings.get(), &core::AppSettings::localInferenceTimeoutMsChanged, this, [this] {
        m_controller->setLocalInferenceTimeoutMs(m_settings->localInferenceTimeoutMs());
    });
    m_controller->setLocalInferenceTemperature(m_settings->localInferenceTemperature());
    connect(m_settings.get(), &core::AppSettings::localInferenceTemperatureChanged, this, [this] {
        m_controller->setLocalInferenceTemperature(m_settings->localInferenceTemperature());
    });
    m_controller->setLocalInferenceTopP(m_settings->localInferenceTopP());
    connect(m_settings.get(), &core::AppSettings::localInferenceTopPChanged, this,
            [this] { m_controller->setLocalInferenceTopP(m_settings->localInferenceTopP()); });
    m_controller->setLocalInferenceMaxTokens(m_settings->localInferenceMaxTokens());
    connect(m_settings.get(), &core::AppSettings::localInferenceMaxTokensChanged, this, [this] {
        m_controller->setLocalInferenceMaxTokens(m_settings->localInferenceMaxTokens());
    });
    m_controller->setPromptContextInjectionEnabled(m_settings->promptContextInjectionEnabled());
    connect(m_settings.get(), &core::AppSettings::promptContextInjectionEnabledChanged, this,
            [this] {
                m_controller->setPromptContextInjectionEnabled(
                    m_settings->promptContextInjectionEnabled());
            });
    m_controller->setSemanticPromptInclusionEnabled(m_settings->semanticPromptInclusionEnabled());
    connect(m_settings.get(), &core::AppSettings::semanticPromptInclusionEnabledChanged, this,
            [this] {
                m_controller->setSemanticPromptInclusionEnabled(
                    m_settings->semanticPromptInclusionEnabled());
            });
    m_controller->setPiperBinaryPath(m_settings->piperBinaryPath());
    connect(m_settings.get(), &core::AppSettings::piperBinaryPathChanged, this,
            [this] { m_controller->setPiperBinaryPath(m_settings->piperBinaryPath()); });
    m_controller->setPiperModelPath(m_settings->piperModelPath());
    connect(m_settings.get(), &core::AppSettings::piperModelPathChanged, this,
            [this] { m_controller->setPiperModelPath(m_settings->piperModelPath()); });
    m_controller->setWhisperBinaryPath(m_settings->whisperBinaryPath());
    connect(m_settings.get(), &core::AppSettings::whisperBinaryPathChanged, this,
            [this] { m_controller->setWhisperBinaryPath(m_settings->whisperBinaryPath()); });
    m_controller->setWhisperModelPath(m_settings->whisperModelPath());
    connect(m_settings.get(), &core::AppSettings::whisperModelPathChanged, this,
            [this] { m_controller->setWhisperModelPath(m_settings->whisperModelPath()); });
    m_controller->setPiperFileOutputExecutionEnabled(m_settings->piperFileOutputExecutionEnabled());
    connect(m_settings.get(), &core::AppSettings::piperFileOutputExecutionEnabledChanged, this,
            [this] {
                m_controller->setPiperFileOutputExecutionEnabled(
                    m_settings->piperFileOutputExecutionEnabled());
            });
    m_controller->setWhisperTranscriptionExecutionEnabled(
        m_settings->whisperTranscriptionExecutionEnabled());
    connect(m_settings.get(), &core::AppSettings::whisperTranscriptionExecutionEnabledChanged, this,
            [this] {
                m_controller->setWhisperTranscriptionExecutionEnabled(
                    m_settings->whisperTranscriptionExecutionEnabled());
            });
    m_controller->setAgentAutonomousMode(m_settings->agentAutonomousMode());
    connect(m_settings.get(), &core::AppSettings::agentAutonomousModeChanged, this,
            [this] { m_controller->setAgentAutonomousMode(m_settings->agentAutonomousMode()); });
    const auto selection = [this] {
        m_controller->setSelectedRuntimeProvider(m_settings->selectedRuntimeProvider());
        m_controller->setSelectedLocalModel(
            m_settings->selectedModelForProvider(m_settings->selectedRuntimeProvider()));
    };
    selection();
    m_localModelRuntime = std::make_unique<LocalModelRuntime>(*m_controller);
    connect(m_settings.get(), &core::AppSettings::selectedRuntimeProviderChanged, this, selection);
    connect(m_settings.get(), &core::AppSettings::selectedLocalModelChanged, this, selection);
    const auto webSearch = [this] {
        m_controller->configureWebSearch(m_settings->webSearchProvider(),
                                         m_settings->webSearchApiKey(),
                                         m_settings->webSearchMaxResults());
    };
    webSearch();
    connect(m_settings.get(), &core::AppSettings::webSearchSettingsChanged, this, webSearch);
    const auto mcp = [this] { m_controller->configureMcpServers(m_settings->mcpServersJson()); };
    mcp();
    connect(m_settings.get(), &core::AppSettings::mcpServersChanged, this, mcp);
    const auto semantic = [this] {
        m_controller->setSemanticProvider(m_settings->semanticProvider(),
                                          m_settings->semanticEmbeddingModel());
    };
    semantic();
    connect(m_settings.get(), &core::AppSettings::semanticSettingsChanged, this, semantic);
    const auto tts = [this] {
        m_controller->configureSpeechTts(m_settings->selectedTtsEngine(),
                                         m_settings->kokoroModelPath(), m_settings->kokoroVoice());
    };
    tts();
    connect(m_settings.get(), &core::AppSettings::selectedTtsEngineChanged, this, tts);
    connect(m_settings.get(), &core::AppSettings::kokoroModelPathChanged, this, tts);
    connect(m_settings.get(), &core::AppSettings::kokoroVoiceChanged, this, tts);
    const auto proxy = [this] {
        if (!m_settings->proxyEnabled()) {
            QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
            return;
        }
        const auto type = m_settings->proxyType() == QStringLiteral("SOCKS5")
                              ? QNetworkProxy::Socks5Proxy
                              : QNetworkProxy::HttpProxy;
        QNetworkProxy::setApplicationProxy(
            QNetworkProxy(type, m_settings->proxyHost(), m_settings->proxyPort(),
                          m_settings->proxyUser(), m_settings->proxyPassword()));
    };
    proxy();
    connect(m_settings.get(), &core::AppSettings::proxySettingsChanged, this, proxy);
    connect(m_settings.get(), &core::AppSettings::cloudApiKeysChanged, this,
            [this] { m_controller->refreshModelDiscovery(); });
    connect(m_settings.get(), &core::AppSettings::selectedCloudProviderChanged, this,
            [this] { m_controller->refreshModelDiscovery(); });
    m_ipcServer.setSettings(m_settings.get());
    m_ipcServer.setController(m_controller.get());
    if (!m_ipcServer.openSessionProjectionStore(
            QFileInfo(m_pathProvider.settingsFilePath()).absolutePath() + "/ipc-sessions.sqlite3"))
        return false;

    m_controller->refreshOllamaStatus();

    startResourceTelemetryIfEnabled();

    // Health check every 60 seconds
    m_healthTimer.start(60000);
    qInfo().noquote() << "Sentinel Daemon Service initialized successfully.";
    return true;
}

void DaemonService::startResourceTelemetryIfEnabled() {
    // Opt-in only: measurement must never become an implicit daemon dependency.
    const auto enabled = qEnvironmentVariable("SENTINEL_RESOURCE_TELEMETRY");
    if (enabled != QStringLiteral("1") &&
        enabled.compare(QStringLiteral("true"), Qt::CaseInsensitive) != 0) {
        return;
    }

    sentinel::core::ResourceMonitorConfig config;
    bool intervalOk = false;
    const int requestedInterval =
        qEnvironmentVariableIntValue("SENTINEL_RESOURCE_TELEMETRY_INTERVAL_MS", &intervalOk);
    if (intervalOk) {
        config.samplingIntervalMs = requestedInterval;
    }

    m_resourceMonitor = std::make_unique<sentinel::core::ResourceMonitor>(
        sentinel::core::makeNativeResourceMonitorAdapter(), nullptr, config);
    if (m_resourceMonitor->start()) {
        qInfo().noquote() << "Resource telemetry monitoring enabled (adapter="
                          << m_resourceMonitor->capabilities().adapterId
                          << ", interval=" << config.effectiveSamplingIntervalMs() << "ms)";
    }
}

void DaemonService::performHealthCheck() {
    if (m_controller) {
        m_controller->refreshOllamaStatus();
    }
}

} // namespace sentinel::daemon
