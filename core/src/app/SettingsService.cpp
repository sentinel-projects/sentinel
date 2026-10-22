// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/SettingsService.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/extension/ExtensionService.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/model/ModelOperationService.h"
#include "sentinel/core/voice/UnifiedAudioService.h"
#include "sentinel/core/security/PermissionService.h"
#include "sentinel/core/security/CredentialStore.h"
#include "sentinel/core/privacy/PrivacyService.h"
#include "sentinel/core/privacy/RetentionPolicy.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/agent/IAgentRunStore.h"
#include "sentinel/core/chat/IConversationStore.h"
#include "sentinel/core/chat/IChatHistoryStore.h"
#include "sentinel/core/interfaces/IMemoryStore.h"
#include "sentinel/core/app/RecoveryService.h"
#include "sentinel/core/app/FileLogger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

namespace sentinel::core {
SettingsService::SettingsService(AppSettings& settings, ModelService* models,
                                 ExtensionService* extensions, VoiceSessionService* audio,
                                 const PermissionService* permissions,
                                 IConversationStore* conversations, IMemoryStore* memory,
                                 IAgentRunStore* agentRuns, IChatHistoryStore* chatHistory,
                                 ModelOperationService* modelOperations)
    : settings_(settings), models_(models), extensions_(extensions), audio_(audio),
      permissions_(permissions), conversations_(conversations), memory_(memory),
      agentRuns_(agentRuns), chatHistory_(chatHistory), modelOperations_(modelOperations) {}

QStringList SettingsService::sectionIds() const {
    const QStringList all{QStringLiteral("general"), QStringLiteral("appearance"),
        QStringLiteral("models"), QStringLiteral("agent"), QStringLiteral("tools"),
        QStringLiteral("extensions"), QStringLiteral("workspaces"),
        QStringLiteral("speech"), QStringLiteral("privacy"), QStringLiteral("network"),
        QStringLiteral("storage"), QStringLiteral("notifications"),
        QStringLiteral("advanced")};
    QSet<int> active;
    for (const auto& row : snapshots()) active.insert(static_cast<int>(row.section));
    QStringList result;
    for (int index = 0; index < all.size(); ++index)
        if (active.contains(index)) result.append(all.at(index));
    return result;
}

QList<SettingSnapshot> SettingsService::snapshots() const {
    QList<SettingSnapshot> rows;
    auto add = [&rows](QString id, SettingSection section, QVariant value, QVariant fallback,
                       QStringList allowed = {}, QStringList keywords = {},
                       SettingScope scope = SettingScope::Global, bool sensitive = false) {
        rows.append({std::move(id), section, std::move(value), std::move(fallback),
                     QStringLiteral("global"), std::move(allowed), std::move(keywords),
                     scope, true, false, sensitive, {}});
    };
    add(QStringLiteral("appearance.theme"), SettingSection::Appearance, settings_.themeName(),
        QStringLiteral("Liquid Glass Light"),
        AppSettings::availableThemes(),
        {QStringLiteral("theme"), QStringLiteral("color")});
    add(QStringLiteral("general.language"), SettingSection::General, settings_.appLanguage(),
        QStringLiteral("en"), settings_.availableLanguages(), {QStringLiteral("locale")});
    add(QStringLiteral("storage.state"), SettingSection::Storage,
        settings_.storageErrorCode().isEmpty() ? QStringLiteral("Ready") : settings_.storageErrorCode(),
        QStringLiteral("Ready"), {}, {QStringLiteral("recovery"), QStringLiteral("corrupt")});
    add(QStringLiteral("notifications.policy"), SettingSection::Notifications,
        settings_.notificationPolicy(), QStringLiteral("Important Only"),
        {QStringLiteral("Disabled"), QStringLiteral("All"),
         QStringLiteral("Custom"), QStringLiteral("Important Only")},
        {QStringLiteral("alerts")});
    add(QStringLiteral("privacy.permission-policy"), SettingSection::Privacy,
        settings_.defaultPermissionPolicyState(), QStringLiteral("Disabled"),
        {QStringLiteral("Disabled"), QStringLiteral("Ask Every Time"),
         QStringLiteral("Enabled"), QStringLiteral("Trusted")},
        {QStringLiteral("approval"), QStringLiteral("authorization")});
    add(QStringLiteral("agent.autonomous-mode"), SettingSection::Agent,
        settings_.agentAutonomousMode(), false, {},
        {QStringLiteral("agent"), QStringLiteral("autonomy")});
    add(QStringLiteral("network.update-check"), SettingSection::Network,
        settings_.updateCheckPolicy(), QStringLiteral("Ask Before Checking"),
        {QStringLiteral("Never"), QStringLiteral("Weekly"),
         QStringLiteral("On Startup"), QStringLiteral("Ask Before Checking")});
    add(QStringLiteral("network.mode"), SettingSection::Network,
        settings_.networkMode(), QStringLiteral("Online"),
        {QStringLiteral("Online"), QStringLiteral("Offline"), QStringLiteral("LocalOnly")},
        {QStringLiteral("connectivity"), QStringLiteral("offline")});
    const auto retention = RetentionPolicy::effective(settings_);
    for (const auto& domain : {QStringLiteral("chat"), QStringLiteral("agentRuns"),
                               QStringLiteral("diagnostics"),
                               QStringLiteral("modelSourceCache"),
                               QStringLiteral("modelOperations")}) {
        SettingSnapshot row{QStringLiteral("privacy.retention.") + domain,
            SettingSection::Privacy, retention.value(domain).toString(),
            RetentionPolicy::defaults().value(domain).toString(),
            QStringLiteral("global"),
            {QStringLiteral("Keep"), QStringLiteral("1d"), QStringLiteral("7d"),
             QStringLiteral("30d"), QStringLiteral("90d")},
            {QStringLiteral("retention"), domain}};
        row.enabled = !retention.isEmpty();
        if (!row.enabled) row.unavailableReason = QStringLiteral("UnsupportedRetentionPolicy");
        rows.append(row);
    }
    add(QStringLiteral("network.web-search-provider"), SettingSection::Network,
        settings_.webSearchProvider(), QStringLiteral("duckduckgo"),
        {QStringLiteral("duckduckgo"), QStringLiteral("exa"), QStringLiteral("parallel")},
        {QStringLiteral("search"), QStringLiteral("network")});
    add(QStringLiteral("network.web-search-max-results"), SettingSection::Network,
        settings_.webSearchMaxResults(), 5, {}, {QStringLiteral("search"), QStringLiteral("limit")});
    add(QStringLiteral("network.web-search-credential-present"), SettingSection::Network,
        !settings_.webSearchApiKey().isEmpty(), false, {},
        {QStringLiteral("search"), QStringLiteral("credential")}, SettingScope::Global, true);
    add(QStringLiteral("network.web-search-credential-state"), SettingSection::Network,
        settings_.credentialState(QStringLiteral("web-search")), QStringLiteral("NotConfigured"), {},
        {QStringLiteral("search"), QStringLiteral("credential")}, SettingScope::Global, true);
    add(QStringLiteral("models.ollama-endpoint"), SettingSection::Advanced,
        settings_.ollamaEndpoint(), QStringLiteral("http://127.0.0.1:11434"), {},
        {QStringLiteral("ollama"), QStringLiteral("endpoint")});
    add(QStringLiteral("models.lm-studio-endpoint"), SettingSection::Advanced,
        settings_.lmStudioEndpoint(), QStringLiteral("http://127.0.0.1:1234"), {},
        {QStringLiteral("lm studio"), QStringLiteral("endpoint")});
    add(QStringLiteral("models.llama-cpp-endpoint"), SettingSection::Advanced,
        settings_.llamaCppEndpoint(), QStringLiteral("http://127.0.0.1:8080"), {},
        {QStringLiteral("llama cpp"), QStringLiteral("endpoint")});
    add(QStringLiteral("workspaces.active"), SettingSection::Workspaces,
        settings_.selectedWorkspaceId(), QStringLiteral("personal"), {},
        {QStringLiteral("project"), QStringLiteral("workspace")});
    if (models_) {
        const auto selected = models_->selectedModel();
        add(QStringLiteral("models.provider"), SettingSection::Models, selected.providerId,
            QStringLiteral("ollama"), models_->knownProviderIds(),
            {QStringLiteral("provider"), QStringLiteral("runtime")});
        add(QStringLiteral("models.model"), SettingSection::Models, selected.modelId,
            QVariant{}, {}, {QStringLiteral("model"), QStringLiteral("inference")});
    }
    if (audio_) {
        add(QStringLiteral("speech.input-device"), SettingSection::Speech,
            audio_->devices()->selectedInputId(), QVariant{},
            audio_->devices()->inputDeviceIds(), {QStringLiteral("microphone")});
        add(QStringLiteral("speech.output-device"), SettingSection::Speech,
            audio_->devices()->selectedOutputId(), QVariant{},
            audio_->devices()->outputDeviceIds(), {QStringLiteral("speaker")});
        add(QStringLiteral("speech.vad-enabled"), SettingSection::Speech,
            audio_->devices()->vadEnabled(), true, {}, {QStringLiteral("voice activity")});
    }
    if (extensions_) {
        for (const auto& item : extensions_->extensions()) {
            SettingSnapshot row{QStringLiteral("extensions.") + item.id,
                SettingSection::Extensions, item.effectiveEnabled, {},
                item.workspacePreference == QLatin1String("inherit")
                    ? QStringLiteral("global") : QStringLiteral("workspace"), {},
                {item.id, item.displayName}, SettingScope::WorkspaceOverride};
            row.unavailableReason = item.available ? QString{} : item.failureCategory;
            rows.append(row);
        }
    }
    const auto profile = workspaceProfile(settings_.selectedWorkspaceId());
    for (const auto& key : {QStringLiteral("providerId"), QStringLiteral("modelId"),
                             QStringLiteral("privacy")}) {
        if (!profile.configured.contains(key)) continue;
        SettingSnapshot row{QStringLiteral("workspaces.") + key,
            SettingSection::Workspaces, profile.configured.value(key).toVariant(), {},
            profile.sources.value(key).toString(QStringLiteral("global")), {},
            {key}, SettingScope::WorkspaceOverride};
        row.unavailableReason = profile.reasons.value(key).toString();
        rows.append(row);
    }
    return rows;
}

SettingActionResult SettingsService::set(const QString& id, const QVariant& value) {
    settings_.clearStorageWriteError();
    auto invalid = [&id](const QString& code) {
        return SettingActionResult{false, code, id, {}};
    };
    auto applied = [this, &id]() {
        const auto error = settings_.storageErrorCode();
        return SettingActionResult{error.isEmpty(), error, id, {}};
    };
    if (!settings_.storageErrorCode().isEmpty())
        return invalid(settings_.storageErrorCode());
    if (id.startsWith(QLatin1String("privacy.retention."))) {
        const auto domain = id.mid(QStringLiteral("privacy.retention.").size());
        if (!RetentionPolicy::set(settings_, domain, value.toString()))
            return invalid(QStringLiteral("settings.invalid-retention-policy"));
        return applied();
    }
    if (id == QLatin1String("agent.autonomous-mode")) {
        if (value.typeId() != QMetaType::Bool)
            return invalid(QStringLiteral("settings.invalid-type"));
        settings_.setAgentAutonomousMode(value.toBool());
        return applied();
    }
    if (id == QLatin1String("network.web-search-max-results")) {
        bool ok = false;
        const auto count = value.toInt(&ok);
        if (!ok || count < 1 || count > 20)
            return invalid(QStringLiteral("settings.invalid-range"));
        settings_.setWebSearchMaxResults(count);
        return applied();
    }
    if (id == QLatin1String("speech.vad-enabled")) {
        if (!audio_ || (value.typeId() != QMetaType::Bool && value.toString() != QLatin1String("true") &&
                        value.toString() != QLatin1String("false")))
            return invalid(QStringLiteral("settings.invalid-type"));
        audio_->devices()->setVadEnabled(value.toBool());
        return applied();
    }
    if (!value.canConvert<QString>()) return invalid(QStringLiteral("settings.invalid-type"));
    const auto text = value.toString().trimmed();
    if (id == QLatin1String("speech.input-device")) {
        if (!audio_ || !audio_->devices()->inputDeviceIds().contains(text) ||
            !audio_->devices()->selectInput(text))
            return invalid(QStringLiteral("settings.unavailable-device"));
    } else if (id == QLatin1String("speech.output-device")) {
        if (!audio_ || !audio_->devices()->outputDeviceIds().contains(text) ||
            !audio_->devices()->selectOutput(text))
            return invalid(QStringLiteral("settings.unavailable-device"));
    } else if (id == QLatin1String("general.language")) {
        if (!settings_.availableLanguages().contains(text))
            return invalid(QStringLiteral("settings.invalid-choice"));
        settings_.setAppLanguage(text);
    } else if (id == QLatin1String("appearance.theme")) {
        if (!AppSettings::availableThemes().contains(text))
            return invalid(QStringLiteral("settings.invalid-choice"));
        settings_.setThemeName(text);
    } else if (id == QLatin1String("notifications.policy")) {
        if (!QStringList{QStringLiteral("Disabled"), QStringLiteral("All"),
                         QStringLiteral("Custom"), QStringLiteral("Important Only")}.contains(text))
            return invalid(QStringLiteral("settings.invalid-choice"));
        settings_.setNotificationPolicy(text);
    } else if (id == QLatin1String("privacy.permission-policy")) {
        if (!QStringList{QStringLiteral("Disabled"), QStringLiteral("Ask Every Time"),
                         QStringLiteral("Enabled"), QStringLiteral("Trusted")}.contains(text))
            return invalid(QStringLiteral("settings.invalid-choice"));
        settings_.setDefaultPermissionPolicyState(text);
    } else if (id == QLatin1String("network.update-check")) {
        if (!QStringList{QStringLiteral("Never"), QStringLiteral("Weekly"),
                         QStringLiteral("On Startup"),
                         QStringLiteral("Ask Before Checking")}.contains(text))
            return invalid(QStringLiteral("settings.invalid-choice"));
        settings_.setUpdateCheckPolicy(text);
    } else if (id == QLatin1String("network.mode")) {
        if (!QStringList{QStringLiteral("Online"), QStringLiteral("Offline"),
                         QStringLiteral("LocalOnly")}.contains(text))
            return invalid(QStringLiteral("settings.invalid-choice"));
        settings_.setNetworkMode(text);
    } else if (id == QLatin1String("network.web-search-provider")) {
        if (!QStringList{QStringLiteral("duckduckgo"), QStringLiteral("exa"),
                         QStringLiteral("parallel")}.contains(text))
            return invalid(QStringLiteral("settings.invalid-choice"));
        settings_.setWebSearchProvider(text);
    } else if (id.endsWith(QStringLiteral("-endpoint"))) {
        const QUrl url(text);
        if (!url.isValid() || !QStringList{QStringLiteral("http"), QStringLiteral("https")}
                                  .contains(url.scheme()) || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || !url.fragment().isEmpty())
            return invalid(QStringLiteral("settings.invalid-endpoint"));
        if (id == QLatin1String("models.ollama-endpoint")) settings_.setOllamaEndpoint(text);
        else if (id == QLatin1String("models.lm-studio-endpoint")) settings_.setLmStudioEndpoint(text);
        else if (id == QLatin1String("models.llama-cpp-endpoint")) settings_.setLlamaCppEndpoint(text);
        else return invalid(QStringLiteral("settings.unknown-setting"));
    } else if (id == QLatin1String("workspaces.active")) {
        if (workspaces_.normalizedWorkspaceId(text, settings_.workspaceCatalogJson()) != text)
            return invalid(QStringLiteral("settings.unknown-workspace"));
        settings_.setSelectedWorkspaceId(text);
    } else if (id == QLatin1String("models.provider")) {
        if (!models_ || !models_->isKnownProvider(text))
            return invalid(QStringLiteral("settings.unknown-provider"));
        models_->setSelectedProviderId(text);
    } else if (id == QLatin1String("models.model")) {
        if (!models_ || text.isEmpty()) return invalid(QStringLiteral("settings.invalid-model"));
        const auto providerId = models_->selectedModel().providerId;
        const auto catalog = models_->providerStatus(providerId);
        if (!catalog.modelIds.contains(text))
            return invalid(QStringLiteral("settings.invalid-model"));
        models_->setSelectedModelId(text);
    } else return invalid(QStringLiteral("settings.unknown-setting"));
    return applied();
}

SettingActionResult SettingsService::reset(const QString& id) {
    for (const auto& row : snapshots())
        if (row.id == id && row.defaultValue.isValid() && !row.sensitive)
            return set(id, row.defaultValue);
    return {false, QStringLiteral("settings.reset-unavailable"), id, {}};
}

QList<SettingActionResult> SettingsService::resetSection(SettingSection section) {
    QList<SettingActionResult> results;
    for (const auto& row : snapshots())
        if (row.section == section && row.scope == SettingScope::Global &&
            row.defaultValue.isValid() && !row.sensitive) results.append(reset(row.id));
    return results;
}

SettingActionResult SettingsService::clearWorkspaceOverride(const QString& workspaceId,
                                                              const QString& key) {
    const auto raw = settings_.workspaceProfilesJson();
    const auto document = QJsonDocument::fromJson(raw.toUtf8());
    if (!document.isObject() ||
        workspaces_.normalizedWorkspaceId(workspaceId, settings_.workspaceCatalogJson()) !=
            workspaceId)
        return {false, QStringLiteral("settings.unknown-workspace"), key, {}};
    auto root = document.object();
    auto workspaces = root.value(QStringLiteral("workspaces")).toObject();
    auto entry = workspaces.value(workspaceId).toObject();
    auto overrides = entry.value(QStringLiteral("overrides")).toObject();
    const auto dot = key.indexOf(QLatin1Char('.'));
    if (dot > 0) {
        const auto group = key.left(dot);
        const auto field = key.mid(dot + 1);
        auto nested = overrides.value(group).toObject();
        if (!nested.contains(field))
            return {false, QStringLiteral("settings.override-absent"), key, {}};
        nested.remove(field);
        if (nested.isEmpty()) overrides.remove(group);
        else overrides.insert(group, nested);
    } else {
        if (!overrides.contains(key))
            return {false, QStringLiteral("settings.override-absent"), key, {}};
        overrides.remove(key);
    }
    entry.insert(QStringLiteral("overrides"), overrides);
    workspaces.insert(workspaceId, entry);
    root.insert(QStringLiteral("workspaces"), workspaces);
    settings_.setWorkspaceProfilesJson(
        QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact)));
    const auto error = settings_.storageErrorCode();
    return {error.isEmpty(), error, key, {}};
}

QJsonObject SettingsService::providerState(const QString& providerId) const {
    if (!models_) return {};
    const auto status = models_->providerStatus(providerId);
    bool credentialPresent = false;
    const auto credentialProvider = providerId == QLatin1String("cloud-api")
        ? settings_.selectedCloudProvider() : providerId;
    if (credentialProvider == QLatin1String("openai")) credentialPresent = !settings_.openAiApiKey().isEmpty();
    else if (credentialProvider == QLatin1String("claude")) credentialPresent = !settings_.claudeApiKey().isEmpty();
    else if (credentialProvider == QLatin1String("gemini")) credentialPresent = !settings_.geminiApiKey().isEmpty();
    else if (credentialProvider == QLatin1String("deepseek")) credentialPresent = !settings_.deepseekApiKey().isEmpty();
    else if (credentialProvider == QLatin1String("groq")) credentialPresent = !settings_.groqApiKey().isEmpty();
    else if (credentialProvider == QLatin1String("mistral")) credentialPresent = !settings_.mistralApiKey().isEmpty();
    const auto selection = models_->selectedModel();
    QString endpoint;
    if (providerId == QLatin1String("ollama")) endpoint = settings_.ollamaEndpoint();
    else if (providerId == QLatin1String("lm-studio")) endpoint = settings_.lmStudioEndpoint();
    else if (providerId == QLatin1String("llama-cpp-server")) endpoint = settings_.llamaCppEndpoint();
    else if (providerId == QLatin1String("cloud-api")) endpoint = settings_.cloudApiEndpoint();
    if (!endpoint.isEmpty()) {
        QUrl safe(endpoint);
        safe.setUserInfo({});
        safe.setQuery(QUrlQuery{});
        safe.setFragment({});
        endpoint = safe.toString();
    }
    const auto metadata = models_->currentModelMetadata(providerId, selection.modelId);
    const auto caps = models_->capabilities(providerId, selection.modelId);
    return {{QStringLiteral("id"), providerId},
            {QStringLiteral("known"), models_->isKnownProvider(providerId)},
            {QStringLiteral("health"), providerHealthName(status.health)},
            {QStringLiteral("catalog"), providerCatalogStateName(status.catalog)},
            {QStringLiteral("discoveryDetail"), status.safeDetail},
            {QStringLiteral("local"), metadata.providerKind == ProviderKind::Local},
            {QStringLiteral("endpoint"), endpoint},
            {QStringLiteral("discoveredModels"), QJsonArray::fromStringList(status.modelIds)},
            {QStringLiteral("contextWindow"), caps.contextWindow.value_or(0)},
            {QStringLiteral("maxOutputTokens"), caps.maxOutputTokens.value_or(0)},
            {QStringLiteral("credentialPresent"), credentialPresent},
            {QStringLiteral("credentialState"), settings_.credentialState(credentialProvider)},
            {QStringLiteral("configuredModel"),
             selection.providerId == providerId ? selection.modelId : QString{}}};
}

QJsonObject SettingsService::extensionState(const QString& extensionId) const {
    if (!extensions_) return {};
    for (const auto& item : extensions_->extensions())
        if (item.id == extensionId)
            return {{QStringLiteral("id"), item.id},
                    {QStringLiteral("globalEnabled"), item.globallyEnabled},
                    {QStringLiteral("workspacePreference"), item.workspacePreference},
                    {QStringLiteral("effectiveEnabled"), item.effectiveEnabled},
                    {QStringLiteral("available"), item.available},
                    {QStringLiteral("health"), static_cast<int>(item.health)},
                    {QStringLiteral("source"), item.source},
                    {QStringLiteral("version"), item.version},
                    {QStringLiteral("credentials"), extensions_->pluginCredentialStates(extensionId)},
                    {QStringLiteral("requirements"), QJsonArray::fromStringList(item.requirements)}};
    return {};
}

SettingActionResult SettingsService::setPluginCredential(const QString& extensionId,
                                                         const QString& credentialId,
                                                         const QString& value) {
    if (!extensions_ || !extensions_->setPluginCredential(extensionId, credentialId, value))
        return {false, QStringLiteral("PluginCredentialWriteFailed"), credentialId, {}};
    return {true, {}, credentialId, {}};
}

SettingActionResult SettingsService::clearPluginCredential(const QString& extensionId,
                                                           const QString& credentialId) {
    if (!extensions_ || !extensions_->clearPluginCredential(extensionId, credentialId))
        return {false, QStringLiteral("PluginCredentialClearFailed"), credentialId, {}};
    return {true, {}, credentialId, {}};
}

SettingActionResult SettingsService::performExtensionAction(const QString& extensionId,
                                                              int action) {
    if (!extensions_ || action < static_cast<int>(ExtensionAction::Enable) ||
        action > static_cast<int>(ExtensionAction::OpenSource))
        return {false, QStringLiteral("settings.invalid-extension-action"), extensionId, {}};
    if (!extensions_->perform(extensionId, static_cast<ExtensionAction>(action)))
        return {false, QStringLiteral("settings.extension-action-failed"), extensionId, {}};
    return {true, {}, extensionId, {}};
}

WorkspaceProfileSnapshot SettingsService::workspaceProfile(const QString& workspaceId) const {
    const auto selection = models_ ? models_->selectedModel() : ModelSelection{};
    return workspaces_.resolveProfile(settings_.workspaceProfilesJson(), workspaceId,
        {{QStringLiteral("providerId"), selection.providerId},
         {QStringLiteral("modelId"), selection.modelId}}, {}, models_, nullptr, extensions_);
}

QJsonObject SettingsService::speechState() const {
    if (!audio_) return {};
    const auto stt = audio_->sttInfo();
    const auto tts = audio_->ttsInfo();
    return {{QStringLiteral("sttProviderId"), stt.id},
            {QStringLiteral("sttModelId"), stt.modelId},
            {QStringLiteral("sttAvailable"), stt.runtimeAvailable && stt.modelAvailable},
            {QStringLiteral("ttsProviderId"), tts.id},
            {QStringLiteral("ttsModelId"), tts.modelId},
            {QStringLiteral("ttsAvailable"), tts.runtimeAvailable && tts.modelAvailable},
            {QStringLiteral("ttsVoices"), QJsonArray::fromStringList(tts.voices)},
            {QStringLiteral("inputDeviceIds"), QJsonArray::fromStringList(audio_->devices()->inputDeviceIds())},
            {QStringLiteral("outputDeviceIds"), QJsonArray::fromStringList(audio_->devices()->outputDeviceIds())},
            {QStringLiteral("selectedInputId"), audio_->devices()->selectedInputId()},
            {QStringLiteral("selectedOutputId"), audio_->devices()->selectedOutputId()},
            {QStringLiteral("vadEnabled"), audio_->devices()->vadEnabled()},
            {QStringLiteral("rawRecordingRetention"), audio_->privacy().retainRawRecordings},
            {QStringLiteral("sttLocal"), stt.local},
            {QStringLiteral("ttsLocal"), tts.local}};
}

QJsonObject SettingsService::securityState() const {
    QJsonArray grants;
    if (permissions_) {
        for (const auto& grant : permissions_->persistentGrants())
            grants.append(QJsonObject{{QStringLiteral("id"), grant.id},
                                      {QStringLiteral("domain"), static_cast<int>(grant.domain)},
                                      {QStringLiteral("access"), static_cast<int>(grant.access)},
                                      {QStringLiteral("scope"), grant.scope},
                                      {QStringLiteral("createdAt"), grant.createdAt.toString(Qt::ISODate)}});
    }
    return {{QStringLiteral("permissionPolicy"), settings_.defaultPermissionPolicyState()},
            {QStringLiteral("persistentGrantCount"),
             grants.size()},
            {QStringLiteral("persistentGrants"), grants},
            {QStringLiteral("permissionServiceAvailable"), permissions_ != nullptr}};
}

QJsonObject SettingsService::privacyState() const {
    auto state = PrivacyService(settings_, models_, audio_, conversations_, memory_, agentRuns_).state();
    QJsonArray pluginCredentials;
    if (extensions_)
        for (const auto& extension : extensions_->extensions())
            if (extension.type == ExtensionType::Plugin)
                for (const auto& credential : extensions_->pluginCredentialStates(extension.id))
                    pluginCredentials.append(credential);
    state.insert(QStringLiteral("pluginCredentials"), pluginCredentials);
    return state;
}

QJsonObject SettingsService::networkState() const {
    const auto secure = defaultCredentialStore().summary();
    const auto effective = NetworkPolicyService::instance().mode();
    return {{QStringLiteral("mode"), settings_.networkMode()},
            {QStringLiteral("effectiveMode"), effective == NetworkMode::Offline
                ? QStringLiteral("Offline") : effective == NetworkMode::LocalOnly
                    ? QStringLiteral("LocalOnly") : QStringLiteral("Online")},
            {QStringLiteral("connectivity"), effective == NetworkMode::Offline
                 ? QStringLiteral("OfflineByUser")
                 : effective == NetworkMode::LocalOnly
                     ? QStringLiteral("LocalOnly") : QStringLiteral("Unverified")},
            {QStringLiteral("proxyEnabled"), settings_.proxyEnabled()},
            {QStringLiteral("proxyType"), settings_.proxyType()},
            {QStringLiteral("proxyHost"), settings_.proxyHost()},
            {QStringLiteral("proxyPort"), settings_.proxyPort()},
            {QStringLiteral("proxyCredentialsConfigured"),
             !settings_.proxyUser().isEmpty() && !settings_.proxyPassword().isEmpty()},
            {QStringLiteral("proxyCredentialState"),
             settings_.credentialState(QStringLiteral("proxy-password"))},
            {QStringLiteral("secureStoreAvailable"),
             secure.status == CredentialStoreStatus::Ready}};
}

QJsonObject SettingsService::recoveryState() const {
    auto state = RecoveryService(conversations_, chatHistory_, agentRuns_).state();
    QJsonArray operations;
    if (modelOperations_) {
        for (const auto& record : modelOperations_->operations()) {
            if (record.state != ModelOperationState::Interrupted || operations.size() >= 100) continue;
            ModelLibraryEntry entry;
            entry.source.id = record.sourceId;
            entry.repositoryId = record.repositoryId;
            entry.artifactFilename = record.artifactFilename;
            entry.revision = record.revision;
            operations.append(QJsonObject{{QStringLiteral("id"), record.id},
                {QStringLiteral("sourceId"), record.sourceId},
                {QStringLiteral("repositoryId"), record.repositoryId},
                {QStringLiteral("filename"), record.artifactFilename},
                {QStringLiteral("revision"), record.revision},
                {QStringLiteral("intendedDestination"),
                    modelOperations_->huggingFaceSource()->destinationPathAtRoot(entry,
                        record.managedStorageRoot.isEmpty()
                            ? modelOperations_->storageManager().activeRoot()
                            : record.managedStorageRoot)},
                {QStringLiteral("progress"), record.progress ? QJsonValue(*record.progress) : QJsonValue()},
                {QStringLiteral("state"), QStringLiteral("Interrupted")}});
        }
    }
    state.insert(QStringLiteral("interruptedModelOperations"), operations);
    if (!operations.isEmpty() && state.value(QStringLiteral("health")).toString() == QLatin1String("Healthy"))
        state.insert(QStringLiteral("health"), QStringLiteral("Recoverable"));
    return state;
}

QJsonObject SettingsService::backupAvailability() const {
    return BackupService(settings_, conversations_, memory_).availability();
}

BackupResult SettingsService::exportBackupJson(const QStringList& domains) const {
    return BackupService(settings_, conversations_, memory_).exportJson(domains);
}

BackupResult SettingsService::importBackupJson(const QByteArray& data,
                                               const QStringList& domains, ImportMode mode) {
    return BackupService(settings_, conversations_, memory_).importJson(data, domains, mode);
}

SettingActionResult SettingsService::clearChatHistory() {
    if (!conversations_ && !chatHistory_)
        return {false, QStringLiteral("ClearChatFailure"), QStringLiteral("chat"), {}};
    if (conversations_ && !conversations_->clearHistory())
        return {false, QStringLiteral("ClearChatFailure"), QStringLiteral("chat"), {}};
    if (chatHistory_) {
        chatHistory_->clear();
        if (!chatHistory_->lastError().isEmpty())
            return {false, QStringLiteral("ClearChatFailure"), QStringLiteral("chat"), {}};
    }
    return {true, {}, QStringLiteral("chat"), {}};
}

SettingActionResult SettingsService::clearMemory() {
    if (!memory_ || !memory_->isAvailable())
        return {false, QStringLiteral("ClearMemoryFailure"), QStringLiteral("memory"), {}};
    memory_->clear();
    if (!memory_->lastError().isEmpty())
        return {false, QStringLiteral("ClearMemoryFailure"), QStringLiteral("memory"), {}};
    return {true, {}, QStringLiteral("memory"), {}};
}

SettingActionResult SettingsService::clearAgentHistory() {
    if (!agentRuns_ || !agentRuns_->clearHistory())
        return {false, QStringLiteral("ClearAgentHistoryFailure"), QStringLiteral("agentRuns"), {}};
    return {true, {}, QStringLiteral("agentRuns"), {}};
}

SettingActionResult SettingsService::clearStaleTemporaryArtifacts() {
    const auto audio = RecoveryService::cleanupTemporaryAudioDetailed();
    const auto speech = RecoveryService::cleanupGeneratedSpeechDetailed();
    const bool failed = audio.value(QStringLiteral("failed")).toInt() > 0 ||
                        speech.value(QStringLiteral("failed")).toInt() > 0;
    if (!failed) RecoveryService::clearCondition(
        QStringLiteral("temporary-data"), QStringLiteral("audio"));
    return {!failed,
            failed
                ? QStringLiteral("PartialCleanupFailure") : QString{},
            QStringLiteral("temporaryArtifacts"),
            {{QStringLiteral("temporaryAudio"), audio},
             {QStringLiteral("generatedSpeech"), speech}}};
}

QJsonObject SettingsService::clearCacheAndTemporaryData() {
    const auto audio = RecoveryService::cleanupTemporaryAudioDetailed();
    const auto speech = RecoveryService::cleanupGeneratedSpeechDetailed();
    const auto model = modelOperations_ ? modelOperations_->clearOwnedCache() : QJsonObject{};
    QJsonArray unavailable;
    if (!modelOperations_) unavailable.append(QStringLiteral("model-cache"));
    const bool partial = !unavailable.isEmpty() ||
        audio.value(QStringLiteral("failed")).toInt() > 0 ||
        speech.value(QStringLiteral("failed")).toInt() > 0 ||
        model.value(QStringLiteral("partialFailure")).toBool();
    if (audio.value(QStringLiteral("failed")).toInt() == 0 &&
        speech.value(QStringLiteral("failed")).toInt() == 0)
        RecoveryService::clearCondition(QStringLiteral("temporary-data"), QStringLiteral("audio"));
    return {{QStringLiteral("sourcesAttempted"), modelOperations_ ? 5 : 2},
            {QStringLiteral("itemsCleared"), audio.value(QStringLiteral("removed")).toInt() +
                speech.value(QStringLiteral("removed")).toInt() +
                model.value(QStringLiteral("itemsCleared")).toInt()},
            {QStringLiteral("temporaryAudio"), audio},
            {QStringLiteral("generatedSpeech"), speech},
            {QStringLiteral("modelCache"), model},
            {QStringLiteral("unavailableSources"), unavailable},
            {QStringLiteral("partialFailure"), partial}};
}

QJsonObject SettingsService::runRetentionMaintenance() {
    auto result = RetentionPolicy::maintain(settings_, conversations_, chatHistory_, agentRuns_);
    const auto policy = RetentionPolicy::effective(settings_);
    if (policy.isEmpty()) {
        RecoveryService::recordCondition(QStringLiteral("retention"),
            QStringLiteral("settings"), QStringLiteral("UnsupportedRetentionPolicy"),
            QStringLiteral("inspect-retention-policy"));
        return result;
    }
    const auto audio = RecoveryService::cleanupTemporaryAudioDetailed();
    const auto speech = RecoveryService::cleanupGeneratedSpeechDetailed();
    result.insert(QStringLiteral("attempted"), result.value(QStringLiteral("attempted")).toInt() + 2);
    result.insert(QStringLiteral("removed"), result.value(QStringLiteral("removed")).toInt() +
        audio.value(QStringLiteral("removed")).toInt() +
        speech.value(QStringLiteral("removed")).toInt());
    if (audio.value(QStringLiteral("failed")).toInt() > 0 ||
        speech.value(QStringLiteral("failed")).toInt() > 0) {
        result.insert(QStringLiteral("partialFailure"), true);
        RecoveryService::recordCondition(QStringLiteral("temporary-data"),
            QStringLiteral("audio"), QStringLiteral("CleanupFailed"),
            QStringLiteral("clear-temporary-data"), QStringLiteral("Degraded"));
    }
    const int logDays = RetentionPolicy::days(policy.value(QStringLiteral("diagnostics")).toString());
    const int logRemoved = FileLogger::instance().applyRetention(logDays);
    result.insert(QStringLiteral("attempted"), result.value(QStringLiteral("attempted")).toInt() + 1);
    if (logRemoved < 0) {
        result.insert(QStringLiteral("partialFailure"), true);
        result.insert(QStringLiteral("diagnosticsFailure"), QStringLiteral("Unavailable"));
    } else result.insert(QStringLiteral("removed"),
        result.value(QStringLiteral("removed")).toInt() + logRemoved);
    const int days = RetentionPolicy::days(policy.value(QStringLiteral("modelSourceCache")).toString());
    if (days) {
        const auto cache = modelOperations_ ? modelOperations_->clearOwnedCache(days) : QJsonObject{};
        result.insert(QStringLiteral("attempted"), result.value(QStringLiteral("attempted")).toInt() + 1);
        result.insert(QStringLiteral("removed"), result.value(QStringLiteral("removed")).toInt() +
            cache.value(QStringLiteral("itemsCleared")).toInt());
        if (!modelOperations_ || cache.value(QStringLiteral("partialFailure")).toBool()) {
            result.insert(QStringLiteral("partialFailure"), true);
            result.insert(QStringLiteral("modelCacheFailure"),
                modelOperations_ ? QStringLiteral("CleanupFailed") : QStringLiteral("Unavailable"));
        }
    }
    const int operationDays = RetentionPolicy::days(
        policy.value(QStringLiteral("modelOperations")).toString());
    if (operationDays) {
        const int count = modelOperations_ ? modelOperations_->pruneOperationHistory(operationDays) : -1;
        result.insert(QStringLiteral("attempted"), result.value(QStringLiteral("attempted")).toInt() + 1);
        if (count < 0) {
            result.insert(QStringLiteral("partialFailure"), true);
            result.insert(QStringLiteral("modelOperationsFailure"), QStringLiteral("Unavailable"));
        } else result.insert(QStringLiteral("removed"),
            result.value(QStringLiteral("removed")).toInt() + count);
    }
    return result;
}

SettingActionResult SettingsService::resolveInterruptedModelOperation(
    const QString& operationId) {
    if (!modelOperations_ || !modelOperations_->resolveInterrupted(operationId))
        return {false, QStringLiteral("ModelRecoveryIncomplete"), operationId, {}};
    return {true, {}, operationId, {}};
}

SettingActionResult SettingsService::clearCredential(const QString& logicalId) {
    if (logicalId == QLatin1String("openai")) settings_.setOpenAiApiKey({});
    else if (logicalId == QLatin1String("claude")) settings_.setClaudeApiKey({});
    else if (logicalId == QLatin1String("gemini")) settings_.setGeminiApiKey({});
    else if (logicalId == QLatin1String("deepseek")) settings_.setDeepseekApiKey({});
    else if (logicalId == QLatin1String("groq")) settings_.setGroqApiKey({});
    else if (logicalId == QLatin1String("mistral")) settings_.setMistralApiKey({});
    else if (logicalId == QLatin1String("web-search")) settings_.setWebSearchApiKey({});
    else if (logicalId == QLatin1String("proxy-user")) settings_.setProxyUser({});
    else if (logicalId == QLatin1String("proxy-password")) settings_.setProxyPassword({});
    else return {false, QStringLiteral("SecretNotFound"), logicalId, {}};
    if (!settings_.storageErrorCode().isEmpty()) {
        return {false, settings_.storageErrorCode(), logicalId, {}};
    }
    if (settings_.credentialState(logicalId) != QLatin1String("NotConfigured"))
        return {false, QStringLiteral("SecretStoreUnavailable"), logicalId, {}};
    return {true, {}, logicalId, {}};
}

SettingActionResult SettingsService::setProviderCredential(const QString& providerId,
                                                           const QString& value) {
    if (value.isEmpty() || value.size() > 8192)
        return {false, QStringLiteral("InvalidCredential"), providerId, {}};
    if (providerId == QLatin1String("openai")) settings_.setOpenAiApiKey(value);
    else if (providerId == QLatin1String("claude")) settings_.setClaudeApiKey(value);
    else if (providerId == QLatin1String("gemini")) settings_.setGeminiApiKey(value);
    else if (providerId == QLatin1String("deepseek")) settings_.setDeepseekApiKey(value);
    else if (providerId == QLatin1String("groq")) settings_.setGroqApiKey(value);
    else if (providerId == QLatin1String("mistral")) settings_.setMistralApiKey(value);
    else if (providerId == QLatin1String("web-search")) settings_.setWebSearchApiKey(value);
    else return {false, QStringLiteral("UnknownProvider"), providerId, {}};
    if (!settings_.storageErrorCode().isEmpty()) {
        return {false, settings_.storageErrorCode(), providerId, {}};
    }
    QString confirmed;
    if (providerId == QLatin1String("openai")) confirmed = settings_.openAiApiKey();
    else if (providerId == QLatin1String("claude")) confirmed = settings_.claudeApiKey();
    else if (providerId == QLatin1String("gemini")) confirmed = settings_.geminiApiKey();
    else if (providerId == QLatin1String("deepseek")) confirmed = settings_.deepseekApiKey();
    else if (providerId == QLatin1String("groq")) confirmed = settings_.groqApiKey();
    else if (providerId == QLatin1String("mistral")) confirmed = settings_.mistralApiKey();
    else if (providerId == QLatin1String("web-search")) confirmed = settings_.webSearchApiKey();
    if (settings_.credentialState(providerId) != QLatin1String("Configured") || confirmed != value)
        return {false, QStringLiteral("SecureStoreUnavailable"), providerId, {}};
    return {true, {}, providerId, {}};
}
} // namespace sentinel::core
