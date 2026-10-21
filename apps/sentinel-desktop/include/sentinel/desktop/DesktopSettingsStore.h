// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/interfaces/ISettingsStore.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include <QSet>

namespace sentinel::desktop {
// UI preferences persist locally; runtime configuration is a daemon projection.
class DesktopSettingsStore final : public core::ISettingsStore {
public:
    DesktopSettingsStore(DesktopRuntimeClient& runtime, const QString& path)
        : m_runtime(runtime), m_preferences(path) {
        m_changed = QObject::connect(&runtime, &DesktopRuntimeClient::changed, &runtime,
                                     [this] { m_writeError.clear(); });
    }
    ~DesktopSettingsStore() override {
        QObject::disconnect(m_changed);
    }
    QString value(const QString& key, const QString& fallback = {}) const override {
        return presentationKey(key) ? m_preferences.value(key, fallback)
                                    : m_runtime.settingsValue(key, fallback);
    }
    void setValue(QString key, QString value) override {
        m_writeError.clear();
        if (presentationKey(key))
            m_preferences.setValue(std::move(key), std::move(value));
        else {
            m_runtime.setSetting(key, value);
            m_writeError =
                m_runtime.ready() ? QStringLiteral("Pending") : QStringLiteral("DaemonUnavailable");
        }
    }
    void remove(const QString& key) override {
        m_writeError.clear();
        if (presentationKey(key))
            m_preferences.remove(key);
    }
    QString errorCode() const override {
        return m_writeError.isEmpty() ? m_preferences.errorCode() : m_writeError;
    }
    bool applyChanges(const QMap<QString, QString>& values,
                      const QStringList& removals = {}) override {
        m_writeError.clear();
        bool local = true;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            local &= presentationKey(it.key());
        }
        for (const auto& key : removals) {
            local &= presentationKey(key);
        }
        if (local) {
            return m_preferences.applyChanges(values, removals);
        }
        if (removals.isEmpty()) {
            QString provider, model;
            for (auto it = values.cbegin(); it != values.cend(); ++it) {
                if (it.key().startsWith("selectedModel.")) {
                    provider = it.key().mid(14);
                    model = it.value();
                }
            }
            if (!provider.isEmpty() && values.size() == (provider == "ollama" ? 2 : 1) &&
                (provider != "ollama" || values.value("selectedLocalModel") == model)) {
                const auto result =
                    m_runtime
                        .settingsService("setProviderModelPreference", {provider, model}, false)
                        .toMap();
                m_writeError = result.value("code", "Pending").toString();
                return false;
            }
        }
        const QSet<QString> workspaceKeys{"workspaceCatalogJson", "selectedWorkspaceId",
                                          "defaultWorkspaceId"};
        QSet<QString> keys;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            keys.insert(it.key());
        }
        if (!removals.isEmpty() || keys != workspaceKeys) {
            m_writeError = "AtomicSettingsUnsupported";
            return false;
        }
        // Default repair is performed by the authoritative daemon from its current catalog.
        const auto result = m_runtime
                                .settingsService("persistWorkspaceState",
                                                 {values.value("workspaceCatalogJson"),
                                                  values.value("selectedWorkspaceId")},
                                                 false)
                                .toMap();
        m_writeError = result.value("code", "Pending").toString();
        return false; // Only the authoritative async reply can update the projected values.
    }
    static bool presentationKey(const QString& key) {
        static const QSet<QString> keys = {"themeName", "coreWardenMode",
                                           "appLanguage",
                                           "activeConversationId",
                                           "companionEnabled",
                                           "quickPanelShortcut",
                                           "quickPanelStartAtLogin",
                                           "developerModeEnabled",
                                           "contextExplainabilityVisible",
                                           "retrievalExplainabilityEnabled",
                                           "onboardingComplete",
                                           "onboardingFlowJson",
                                           "onboardingUseCase",
                                           "onboardingAiProvider",
                                           "selectedSystemMode",
                                           "recoveryDraftText",
                                           "reducedMotionEnabled",
                                           "reducedTransparencyEnabled",
                                           "highContrastEnabled",
                                           "uiDensity",
                                           "notificationCenterJson",
                                           "notificationPolicy",
                                           "notifyModelDownloads",
                                           "notifyModelRemovals",
                                           "notifyAgentResponses",
                                           "notifySystemUpdates",
                                           "soundEffectsEnabled",
                                           "updateCheckPolicy",
                                           "updateCheckUrl",
                                           "updateWorkflowState"};
        return keys.contains(key);
    }

private:
    QMetaObject::Connection m_changed;
    QString m_writeError;
    DesktopRuntimeClient& m_runtime;
    core::JsonSettingsStore m_preferences;
};
} // namespace sentinel::desktop
