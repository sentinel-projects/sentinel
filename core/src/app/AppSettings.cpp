// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/app/AppSettings.h"
#include <QThread>

#include "sentinel/core/model/ModelRegistry.h"
#include "sentinel/core/model/ModelRouting.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/app/WorkspaceService.h"
#include "sentinel/core/runtime/OllamaRuntime.h"

#include <QDir>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QStandardPaths>
#include <algorithm>

namespace sentinel::core {

namespace {

bool isKnownRuntimeProviderId(const QString& providerId) {
    return providerId == QStringLiteral("ollama") ||
           providerId == QStringLiteral("openai-compatible-local") ||
           providerId == QStringLiteral("lm-studio") ||
           providerId == QStringLiteral("llama-cpp-server") ||
           providerId == QStringLiteral("openai-compatible") ||
           providerId == QStringLiteral("cloud-api") || providerId == QStringLiteral("openai") ||
           providerId == QStringLiteral("deepseek") || providerId == QStringLiteral("groq") ||
           providerId == QStringLiteral("mistral") || providerId == QStringLiteral("claude") ||
           providerId == QStringLiteral("gemini");
}

bool isKnownSkillProfileId(const QString& profileId) {
    return profileId == QStringLiteral("developer") || profileId == QStringLiteral("student") ||
           profileId == QStringLiteral("researcher") ||
           profileId == QStringLiteral("personal-assistant") ||
           profileId == QStringLiteral("custom");
}

bool isKnownModelRoleId(const QString& roleId) {
    return modelRoleIds().contains(roleId.trimmed().toLower());
}

QString normalizedPermissionPolicyState(const QString& state) {
    const auto normalized = state.trimmed().toLower();
    if (normalized == QStringLiteral("ask every time") ||
        normalized == QStringLiteral("ask-every-time")) {
        return QStringLiteral("Ask Every Time");
    }
    if (normalized == QStringLiteral("trusted")) {
        return QStringLiteral("Trusted");
    }
    if (normalized == QStringLiteral("enabled")) {
        return QStringLiteral("Enabled");
    }
    return QStringLiteral("Disabled");
}

QString normalizedUpdatePolicy(const QString& policy) {
    const auto normalized = policy.trimmed().toLower();
    if (normalized == QStringLiteral("never")) {
        return QStringLiteral("Never");
    }
    if (normalized == QStringLiteral("weekly")) {
        return QStringLiteral("Weekly");
    }
    if (normalized == QStringLiteral("on startup") || normalized == QStringLiteral("startup")) {
        return QStringLiteral("On Startup");
    }
    return QStringLiteral("Ask Before Checking");
}

QString normalizedNotificationPolicy(const QString& policy) {
    const auto normalized = policy.trimmed().toLower();
    if (normalized == QStringLiteral("disabled")) {
        return QStringLiteral("Disabled");
    }
    if (normalized == QStringLiteral("all")) {
        return QStringLiteral("All");
    }
    if (normalized == QStringLiteral("custom")) {
        return QStringLiteral("Custom");
    }
    return QStringLiteral("Important Only");
}

QString normalizedOnboardingUseCase(const QString& useCase) {
    const auto normalized = useCase.trimmed().toLower();
    if (normalized == QStringLiteral("coding")) {
        return QStringLiteral("Coding");
    }
    if (normalized == QStringLiteral("study")) {
        return QStringLiteral("Study");
    }
    if (normalized == QStringLiteral("writing")) {
        return QStringLiteral("Writing");
    }
    if (normalized == QStringLiteral("research")) {
        return QStringLiteral("Research");
    }
    if (normalized == QStringLiteral("creative")) {
        return QStringLiteral("Creative");
    }
    if (normalized == QStringLiteral("business")) {
        return QStringLiteral("Business");
    }
    if (normalized == QStringLiteral("personal assistant")) {
        return QStringLiteral("Personal Assistant");
    }
    return QStringLiteral("General Assistant");
}

QString normalizedOnboardingAiProvider(const QString& provider) {
    const auto normalized = provider.trimmed().toLower();
    if (normalized == QStringLiteral("lm studio") || normalized == QStringLiteral("lm-studio")) {
        return QStringLiteral("LM Studio");
    }
    if (normalized == QStringLiteral("llama.cpp server") ||
        normalized == QStringLiteral("llama-cpp-server")) {
        return QStringLiteral("llama.cpp server");
    }
    if (normalized == QStringLiteral("openai-compatible local endpoint") ||
        normalized == QStringLiteral("openai-compatible-local")) {
        return QStringLiteral("OpenAI-compatible local endpoint");
    }
    if (normalized == QStringLiteral("cloud api") || normalized == QStringLiteral("cloud-api")) {
        return QStringLiteral("Cloud API");
    }
    return QStringLiteral("Ollama");
}

QString normalizedUiDensity(const QString& density) {
    const auto normalized = density.trimmed().toLower();
    if (normalized == QStringLiteral("compact")) {
        return QStringLiteral("Compact");
    }
    if (normalized == QStringLiteral("large")) {
        return QStringLiteral("Large");
    }
    return QStringLiteral("Comfortable");
}

QString normalizedUpdateWorkflowState(const QString& state) {
    const auto normalized = state.trimmed();
    return normalized.isEmpty() ? QStringLiteral("Not Checked") : normalized;
}

QString normalizedWorkspaceIdSetting(const QString& workspaceId) {
    const auto normalized = workspaceId.trimmed();
    return normalized.isEmpty() ? QStringLiteral("personal") : normalized;
}

QString normalizedAttachmentBehavior(const QString& behavior) {
    const auto normalized = behavior.trimmed().toLower();
    if (normalized == QStringLiteral("replace") ||
        normalized == QStringLiteral("replace existing attachment")) {
        return QStringLiteral("Replace Existing Attachment");
    }
    if (normalized == QStringLiteral("paste enabled") || normalized == QStringLiteral("paste") ||
        normalized == QStringLiteral("paste attachment enabled")) {
        return QStringLiteral("Paste Attachment Enabled");
    }
    return QStringLiteral("Manual Attachments Only");
}

QString normalizedExportFormat(const QString& format) {
    const auto normalized = format.trimmed().toLower();
    if (normalized == QStringLiteral("pdf")) {
        return QStringLiteral("PDF");
    }
    if (normalized == QStringLiteral("txt") || normalized == QStringLiteral("text")) {
        return QStringLiteral("TXT");
    }
    if (normalized == QStringLiteral("docx")) {
        return QStringLiteral("DOCX");
    }
    if (normalized == QStringLiteral("json")) {
        return QStringLiteral("JSON");
    }
    return QStringLiteral("Markdown");
}

} // namespace

AppSettings::AppSettings(std::unique_ptr<ISettingsStore> store, QObject* parent)
    : AppSettings(std::move(store), platformCredentialStore(), parent) {}

AppSettings::AppSettings(std::unique_ptr<ISettingsStore> store, CredentialStore credentials,
                         QObject* parent)
    : QObject(parent), store_(std::move(store)), credentialStore_(std::move(credentials)) {
    applyNetworkPolicy();
}

void AppSettings::applyNetworkPolicy() const {
    const auto requested = networkMode();
    if (requested == QLatin1String("Offline")) {
        NetworkPolicyService::instance().setMode(NetworkMode::Offline);
        return;
    }
    const auto profile = WorkspaceService{}.resolveProfile(workspaceProfilesJson(),
                                                            selectedWorkspaceId());
    const bool workspaceLocal = profile.configured.value(QStringLiteral("privacy")).toString() ==
                                QLatin1String("local-only");
    NetworkPolicyService::instance().setMode(
        requested == QLatin1String("LocalOnly") || workspaceLocal
            ? NetworkMode::LocalOnly : NetworkMode::Online);
}

QString AppSettings::credential(const QString& id, const QString& legacyKey) const {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return {};
    }
    const CredentialKey key{id, QStringLiteral("apiKey")};
    const auto stored = credentialStore_.readCredential(key);
    auto legacy = store_ ? store_->value(legacyKey) : QString{};
    if (legacy.isEmpty()) {
        return stored.secret.value_or(QString{});
    }
    auto writer = admitSettingsWrite();
    if (!lastWriteError_.isEmpty()) {
        return stored.secret.value_or(legacy);
    }
    if (stored.secret.has_value()) {
        store_->remove(legacyKey);
        return *stored.secret;
    }
    const auto written = credentialStore_.storeCredential(key, legacy);
    if (!written.succeeded) {
        return legacy;
    }
    const auto confirmed = credentialStore_.readCredential(key);
    if (!confirmed.secret.has_value() || *confirmed.secret != legacy) return legacy;
    store_->remove(legacyKey);
    return legacy;
}

bool AppSettings::setCredential(const QString& id, const QString& legacyKey,
                                const QString& value) {
    auto writer = admitSettingsWrite();
    if (wrongThreadWrite_ || !lastWriteError_.isEmpty()) {
        return false;
    }
    if (!store_->errorCode().isEmpty()) {
        lastWriteError_ = store_->errorCode();
        return false;
    }
    const CredentialKey key{id, QStringLiteral("apiKey")};
    const QString normalized = value;
    const auto current = credentialStore_.readCredential(key);
    if (current.secret.has_value() && normalized == *current.secret) {
        return true;
    }
    if (normalized.isEmpty()) {
        if (credentialStore_.summary().status != CredentialStoreStatus::Ready) return false;
        const auto removed = credentialStore_.deleteCredential(key);
        if (!removed.succeeded && credentialStore_.containsCredential(key).succeeded) return false;
        if (store_) store_->remove(legacyKey);
        return !store_ || store_->errorCode().isEmpty();
    }
    const auto written = credentialStore_.storeCredential(key, normalized);
    if (!written.succeeded) return false;
    const auto confirmed = credentialStore_.readCredential(key);
    if (!confirmed.secret.has_value() || *confirmed.secret != normalized) return false;
    if (store_) store_->remove(legacyKey);
    return !store_ || store_->errorCode().isEmpty();
}

QString AppSettings::credentialState(const QString& logicalId) const {
    const QHash<QString, QString> legacy{
        {QStringLiteral("openai"), QString::fromLatin1(openAiApiKeyKey)},
        {QStringLiteral("claude"), QString::fromLatin1(claudeApiKeyKey)},
        {QStringLiteral("gemini"), QString::fromLatin1(geminiApiKeyKey)},
        {QStringLiteral("deepseek"), QString::fromLatin1(deepseekApiKeyKey)},
        {QStringLiteral("groq"), QString::fromLatin1(groqApiKeyKey)},
        {QStringLiteral("mistral"), QString::fromLatin1(mistralApiKeyKey)},
        {QStringLiteral("web-search"), QString::fromLatin1(webSearchApiKeyKey)},
        {QStringLiteral("proxy-user"), QString::fromLatin1(proxyUserKey)},
        {QStringLiteral("proxy-password"), QString::fromLatin1(proxyPasswordKey)}};
    if (!legacy.contains(logicalId)) return QStringLiteral("Unavailable");
    if (credentialStore_.containsCredential(CredentialKey{logicalId, QStringLiteral("apiKey")})
            .succeeded) return QStringLiteral("Configured");
    if (store_ && !store_->value(legacy.value(logicalId)).isEmpty())
        return QStringLiteral("MigrationRequired");
    return credentialStore_.summary().status == CredentialStoreStatus::Ready
        ? QStringLiteral("NotConfigured") : QStringLiteral("SecureStoreUnavailable");
}

QString AppSettings::storageErrorCode() const {
    if (wrongThreadWrite_) {
        return QStringLiteral("SettingsWrongThread");
    }
    if (!lastWriteError_.isEmpty()) {
        return lastWriteError_;
    }
    return store_ ? store_->errorCode() : QStringLiteral("StoreUnavailable");
}
void AppSettings::clearStorageWriteError() {
    if (QThread::currentThread() != thread()) {
        return;
    }
    wrongThreadWrite_ = false;
    lastWriteError_.clear();
}
bool AppSettings::attachStorageAdmission(std::shared_ptr<StorageAdmission> admission) {
    if (QThread::currentThread() != thread() || !admission) {
        return false;
    }
    if (storageAdmission_) {
        return storageAdmission_ == admission;
    }
    storageGeneration_ = admission->snapshot().generation;
    storageAdmission_ = std::move(admission);
    return true;
}
StorageAdmission::Writer AppSettings::admitSettingsWrite() const {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return {};
    }
    wrongThreadWrite_ = false;
    lastWriteError_.clear();
    if (persistenceReadOnly_) {
        lastWriteError_ = QStringLiteral("SettingsReadOnly");
        return {};
    }
    if (!store_) {
        lastWriteError_ = QStringLiteral("StoreUnavailable");
        return {};
    }
    if (!storageAdmission_) {
        return {};
    }
    auto writer = storageAdmission_->admit(storageGeneration_);
    if (!writer) {
        const auto state = storageAdmission_->snapshot();
        lastWriteError_ = state.state == StorageAdmission::State::Unavailable
                              ? QStringLiteral("StorageUnavailable")
                          : state.generation != storageGeneration_
                              ? QStringLiteral("StaleStorageGeneration")
                              : QStringLiteral("StorageBusy");
    }
    return writer;
}
bool AppSettings::persistValue(QString key, QString value) const {
    auto writer = admitSettingsWrite();
    if (wrongThreadWrite_ || !lastWriteError_.isEmpty()) {
        return false;
    }
    store_->setValue(std::move(key), std::move(value));
    return store_->errorCode().isEmpty();
}
bool AppSettings::removeValue(const QString& key) const {
    auto writer = admitSettingsWrite();
    if (wrongThreadWrite_ || !lastWriteError_.isEmpty()) {
        return false;
    }
    store_->remove(key);
    return store_->errorCode().isEmpty();
}
bool AppSettings::persistValues(const QMap<QString, QString>& values) const {
    auto writer = admitSettingsWrite();
    if (wrongThreadWrite_ || !lastWriteError_.isEmpty()) {
        return false;
    }
    if (store_->applyChanges(values)) {
        return true;
    }
    lastWriteError_ = store_->errorCode().isEmpty() ? QStringLiteral("AtomicSettingsUnsupported")
                                                    : store_->errorCode();
    return false;
}
bool AppSettings::applyPortableSettings(const QString& theme, const QString& language,
                                        const QString& network) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return false;
    }
    if (!availableThemes().contains(theme) || !availableLanguages().contains(language) ||
        !QStringList{"Online", "Offline", "LocalOnly"}.contains(network)) {
        lastWriteError_ = "InvalidSettings";
        return false;
    }
    const auto oldTheme = themeName(), oldLanguage = appLanguage(), oldNetwork = networkMode();
    if (!persistValues({{QString::fromLatin1(themeNameKey), theme},
                        {QString::fromLatin1(appLanguageKey), language},
                        {QString::fromLatin1(networkModeKey), network}})) {
        return false;
    }
    applyNetworkPolicy();
    if (oldTheme != theme) {
        emit themeNameChanged();
    }
    if (oldLanguage != language) {
        emit appLanguageChanged();
    }
    if (oldNetwork != network) {
        emit networkModeChanged();
    }
    return true;
}
bool AppSettings::persistWorkspaceState(const QString& catalog, const QString& selection) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return false;
    }
    const auto doc = QJsonDocument::fromJson(catalog.toUtf8());
    WorkspaceService workspaces;
    if (!doc.isObject() || doc.object().value("version").toInt() != 1 ||
        !doc.object().value("customWorkspaces").isArray() ||
        workspaces.normalizedWorkspaceId(selection, catalog) != selection) {
        lastWriteError_ = QStringLiteral("InvalidWorkspaceState");
        return false;
    }
    const auto previousSelection = selectedWorkspaceId();
    const auto previousDefault = defaultWorkspaceId();
    const auto normalizedDefault = workspaces.normalizedWorkspaceId(previousDefault, catalog);
    const auto nextDefault = workspaces.selectedWorkspace(normalizedDefault, catalog).archived
                                 ? QStringLiteral("personal")
                                 : normalizedDefault;
    const auto nextSelection = workspaces.selectedWorkspace(selection, catalog).archived
                                   ? QStringLiteral("personal")
                                   : selection;
    if (!persistValues({{QString::fromLatin1(workspaceCatalogJsonKey), catalog},
                        {QString::fromLatin1(selectedWorkspaceIdKey), nextSelection},
                        {QString::fromLatin1(defaultWorkspaceIdKey), nextDefault}})) {
        return false;
    }
    applyNetworkPolicy();
    if (previousSelection != nextSelection) {
        emit selectedWorkspaceIdChanged();
    }
    emit workspaceSettingsChanged();
    return true;
}

QString AppSettings::retentionPolicyJson() const {
    return store_ ? store_->value(QStringLiteral("privacy.retentionPolicy.v1")) : QString{};
}

void AppSettings::setRetentionPolicyJson(const QString& json) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (store_) {
        if (!persistValue(QStringLiteral("privacy.retentionPolicy.v1"), json)) {
            return;
        }
    }
}

QStringList AppSettings::availableThemes() {
    return {QStringLiteral("Liquid Glass Light"), QStringLiteral("Paper"), QStringLiteral("Linen"),
            QStringLiteral("Sage Light"), QStringLiteral("Sky Light"), QStringLiteral("Solarized Light"),
            QStringLiteral("Liquid Glass Dark"), QStringLiteral("Sentinel Classic"),
            QStringLiteral("Midnight Blue"), QStringLiteral("Aurora Teal"), QStringLiteral("Graphite Grey"),
            QStringLiteral("Nord Frost"), QStringLiteral("Dracula"), QStringLiteral("Tokyo Night")};
}

QString AppSettings::themeName() const {
    const auto fallback = QString::fromLatin1(defaultThemeName);
    return store_ ? store_->value(QString::fromLatin1(themeNameKey), fallback) : fallback;
}

void AppSettings::setThemeName(const QString& themeName) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = themeName.trimmed();
    if (normalized.isEmpty() || normalized == this->themeName() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(themeNameKey), normalized)) {
        return;
    }
    emit themeNameChanged();
}

QString AppSettings::configurationProfile() const {
    const auto fallback = QString::fromLatin1(defaultConfigurationProfile);
    return store_ ? store_->value(QString::fromLatin1(configurationProfileKey), fallback)
                  : fallback;
}

void AppSettings::setConfigurationProfile(const QString& configurationProfile) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = configurationProfile.trimmed();
    if (normalized.isEmpty() || normalized == this->configurationProfile() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(configurationProfileKey), normalized)) {
        return;
    }
    emit configurationProfileChanged();
}

QString AppSettings::appLanguage() const {
    const auto systemLanguage = QLocale::system().name().left(2).toLower();
    const auto fallback =
        (systemLanguage == QStringLiteral("tr")) ? QStringLiteral("tr") : QStringLiteral("en");
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(appLanguageKey), fallback) : fallback;
    const auto normalized = stored.trimmed().toLower();
    return availableLanguages().contains(normalized) ? normalized : fallback;
}

void AppSettings::setAppLanguage(const QString& language) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = language.trimmed().toLower();
    const auto systemLanguage = QLocale::system().name().left(2).toLower();
    const auto fallback =
        (systemLanguage == QStringLiteral("tr")) ? QStringLiteral("tr") : QStringLiteral("en");
    const auto selected = availableLanguages().contains(normalized) ? normalized : fallback;
    if (selected == appLanguage() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(appLanguageKey), selected)) {
        return;
    }
    emit appLanguageChanged();
}

QStringList AppSettings::availableLanguages() const {
    QStringList result{QStringLiteral("en"), QStringLiteral("tr")};
    const auto files = QDir(QStringLiteral(":/i18n"))
                           .entryList({QStringLiteral("sentinel_*.qm")}, QDir::Files);
    for (const auto& file : files) {
        const auto code = file.mid(9, file.size() - 12);
        if (!code.isEmpty() && !result.contains(code)) result.append(code);
    }
    return result;
}

QString AppSettings::languageDisplayName(const QString& language) const {
    const auto normalized = language.trimmed().toLower();
    if (normalized == QStringLiteral("tr")) {
        return tr("Türkçe");
    }
    if (normalized == QStringLiteral("de")) {
        return tr("Deutsch");
    }
    if (normalized == QStringLiteral("es")) {
        return tr("Español");
    }
    if (normalized == QStringLiteral("fr")) {
        return tr("Français");
    }
    if (normalized == QStringLiteral("zh")) {
        return tr("中文");
    }
    if (normalized == QStringLiteral("ja")) {
        return tr("日本語");
    }
    if (normalized == QStringLiteral("ar")) {
        return tr("العربية");
    }
    return tr("English");
}

QString AppSettings::routingModeName() const {
    const auto fallback = sentinel::core::routingModeName(RoutingMode::LocalOnly);
    return store_ ? normalizedRoutingModeName(
                        store_->value(QString::fromLatin1(routingModeKey), fallback))
                  : fallback;
}

void AppSettings::setRoutingModeName(const QString& routingModeName) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = normalizedRoutingModeName(routingModeName);
    if (normalized == this->routingModeName() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(routingModeKey), normalized)) {
        return;
    }
    emit routingModeNameChanged();
}

QStringList AppSettings::availableRoutingModes() const {
    return routingModeNames();
}

QString AppSettings::ollamaEndpoint() const {
    const auto fallback = OllamaEndpoint::defaultEndpoint().toString();
    return store_ ? OllamaEndpoint::fromUserInput(
                        store_->value(QString::fromLatin1(ollamaEndpointKey), fallback))
                        .toString()
                  : fallback;
}

void AppSettings::setOllamaEndpoint(const QString& endpoint) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = OllamaEndpoint::fromUserInput(endpoint).toString();
    if (normalized == ollamaEndpoint() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(ollamaEndpointKey), normalized)) {
        return;
    }
    emit ollamaEndpointChanged();
}

QString AppSettings::lmStudioEndpoint() const {
    if (!store_)
        return QStringLiteral("http://127.0.0.1:1234");
    return store_->value(QString::fromLatin1(lmStudioEndpointKey),
                         QStringLiteral("http://127.0.0.1:1234"));
}

void AppSettings::setLmStudioEndpoint(const QString& endpoint) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (endpoint == lmStudioEndpoint() || !store_)
        return;
    if (!persistValue(QString::fromLatin1(lmStudioEndpointKey), endpoint)) {
        return;
    }
    emit lmStudioEndpointChanged();
}

QString AppSettings::llamaCppEndpoint() const {
    if (!store_)
        return QStringLiteral("http://127.0.0.1:8080");
    return store_->value(QString::fromLatin1(llamaCppEndpointKey),
                         QStringLiteral("http://127.0.0.1:8080"));
}

void AppSettings::setLlamaCppEndpoint(const QString& endpoint) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (endpoint == llamaCppEndpoint() || !store_)
        return;
    if (!persistValue(QString::fromLatin1(llamaCppEndpointKey), endpoint)) {
        return;
    }
    emit llamaCppEndpointChanged();
}

QString AppSettings::cloudApiEndpoint() const {
    if (!store_)
        return {};
    return store_->value(QString::fromLatin1(cloudApiEndpointKey), {});
}

void AppSettings::setCloudApiEndpoint(const QString& endpoint) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (endpoint == cloudApiEndpoint() || !store_)
        return;
    if (!persistValue(QString::fromLatin1(cloudApiEndpointKey), endpoint)) {
        return;
    }
    emit cloudApiEndpointChanged();
}

QString AppSettings::webSearchProvider() const {
    const auto fallback = QString::fromLatin1(defaultWebSearchProvider);
    const auto value =
        store_ ? store_->value(QString::fromLatin1(webSearchProviderKey), fallback) : fallback;
    const auto normalized = value.trimmed().toLower();
    return (normalized == QStringLiteral("exa") || normalized == QStringLiteral("parallel") ||
            normalized == QStringLiteral("duckduckgo"))
               ? normalized
               : fallback;
}

void AppSettings::setWebSearchProvider(const QString& provider) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = provider.trimmed().toLower();
    const auto selected =
        (normalized == QStringLiteral("parallel") || normalized == QStringLiteral("duckduckgo"))
            ? normalized
            : QString::fromLatin1(defaultWebSearchProvider);
    if (selected == webSearchProvider() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(webSearchProviderKey), selected)) {
        return;
    }
    emit webSearchSettingsChanged();
}

QString AppSettings::webSearchApiKey() const {
    return credential(QStringLiteral("web-search"), QString::fromLatin1(webSearchApiKeyKey));
}

void AppSettings::setWebSearchApiKey(const QString& key) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("web-search"), QString::fromLatin1(webSearchApiKeyKey), key))
        emit webSearchSettingsChanged();
}

QString AppSettings::mcpServersJson() const {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return {};
    }
    if (!store_) return {};
    const auto raw = store_->value(QString::fromLatin1(mcpServersJsonKey));
    if (raw.isEmpty()) return {};
    bool complete = false;
    auto safe = secureMcpServersJson(raw, &complete, false);
    if (complete && safe == raw) {
        return safe;
    }
    auto writer = admitSettingsWrite();
    if (!lastWriteError_.isEmpty()) {
        return safe;
    }
    safe = secureMcpServersJson(raw, &complete);
    if (complete && safe != raw) store_->setValue(QString::fromLatin1(mcpServersJsonKey), safe);
    return safe;
}

void AppSettings::setMcpServersJson(const QString& json) {
    auto writer = admitSettingsWrite();
    if (wrongThreadWrite_ || !lastWriteError_.isEmpty()) {
        return;
    }
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_) return;
    bool complete = false;
    const auto safe = secureMcpServersJson(json, &complete);
    if (!complete || safe == store_->value(QString::fromLatin1(mcpServersJsonKey))) {
        return;
    }
    store_->setValue(QString::fromLatin1(mcpServersJsonKey), safe);
    if (!store_->errorCode().isEmpty()) {
        return;
    }
    emit mcpServersChanged();
}

QString AppSettings::secureMcpServersJson(const QString& json, bool* complete, bool migrate) const {
    *complete = false;
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    if (!document.isObject()) return QStringLiteral("{}");
    auto root = document.object();
    const bool wrapped = root.value(QStringLiteral("mcpServers")).isObject();
    auto servers = wrapped ? root.value(QStringLiteral("mcpServers")).toObject() : root;
    bool allStored = true;
    for (auto it = servers.begin(); it != servers.end(); ++it) {
        auto server = it.value().toObject();
        if (server.isEmpty()) continue;
        auto headers = server.value(QStringLiteral("headers")).toObject();
        for (auto header = headers.begin(); header != headers.end(); ++header) {
            if (!header.value().isString()) continue;
            const QByteArray identity = (it.key() + QLatin1Char(':') + header.key()).toUtf8();
            const auto id = QStringLiteral("mcp-") + QString::fromLatin1(
                QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex());
            const auto reference = QStringLiteral("secret://") + id;
            const auto value = header.value().toString();
            if (value == reference || value.isEmpty()) continue;
            if (value.startsWith(QLatin1String("secret://"))) {
                allStored = false;
                header.value() = QString();
                continue;
            }
            if (!migrate) {
                allStored = false;
                header.value() = QString();
                continue;
            }
            const CredentialKey key{id, QStringLiteral("header")};
            const auto write = credentialStore_.storeCredential(key, value);
            const auto read = write.succeeded ? credentialStore_.readCredential(key)
                                              : CredentialReadResult{};
            if (!read.secret.has_value() || *read.secret != value) {
                allStored = false;
                header.value() = QString(); // Never return legacy secret in a settings snapshot.
            } else {
                header.value() = reference;
            }
        }
        server.insert(QStringLiteral("headers"), headers);
        it.value() = server;
    }
    if (wrapped) root.insert(QStringLiteral("mcpServers"), servers);
    else root = servers;
    *complete = allStored;
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

int AppSettings::webSearchMaxResults() const {
    bool ok = false;
    const auto value =
        store_ ? store_->value(QString::fromLatin1(webSearchMaxResultsKey), QStringLiteral("5"))
                     .toInt(&ok)
               : 5;
    return ok ? std::clamp(value, 1, 20) : 5;
}

void AppSettings::setWebSearchMaxResults(int maxResults) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = std::clamp(maxResults, 1, 20);
    if (normalized == webSearchMaxResults() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(webSearchMaxResultsKey), QString::number(normalized))) {
        return;
    }
    emit webSearchSettingsChanged();
}

QString AppSettings::semanticProvider() const {
    const auto fallback = QString::fromLatin1(defaultSemanticProvider);
    const auto value =
        store_ ? store_->value(QString::fromLatin1(semanticProviderKey), fallback) : fallback;
    return value.trimmed().toLower() == QStringLiteral("ollama") ? QStringLiteral("ollama")
                                                                 : QStringLiteral("disabled");
}

void AppSettings::setSemanticProvider(const QString& provider) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = provider.trimmed().toLower() == QStringLiteral("ollama")
                              ? QStringLiteral("ollama")
                              : QStringLiteral("disabled");
    if (selected == semanticProvider() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(semanticProviderKey), selected)) {
        return;
    }
    emit semanticSettingsChanged();
}

QString AppSettings::semanticEmbeddingModel() const {
    const auto fallback = QString::fromLatin1(defaultSemanticEmbeddingModel);
    return store_
               ? store_->value(QString::fromLatin1(semanticEmbeddingModelKey), fallback).trimmed()
               : fallback;
}

void AppSettings::setSemanticEmbeddingModel(const QString& model) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = model.trimmed();
    if (normalized.isEmpty() || normalized == semanticEmbeddingModel() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(semanticEmbeddingModelKey), normalized)) {
        return;
    }
    emit semanticSettingsChanged();
}

QString AppSettings::selectedRuntimeProvider() const {
    const auto fallback = QString::fromLatin1(defaultSelectedRuntimeProvider);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(selectedRuntimeProviderKey), fallback)
               : fallback;
    const auto normalized = stored.trimmed().toLower();
    if (isKnownRuntimeProviderId(normalized)) {
        return normalized;
    }
    return fallback;
}

void AppSettings::setSelectedRuntimeProvider(const QString& providerId) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = providerId.trimmed().toLower();
    const auto selected = isKnownRuntimeProviderId(normalized)
                              ? normalized
                              : QString::fromLatin1(defaultSelectedRuntimeProvider);
    if (selected == selectedRuntimeProvider() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(selectedRuntimeProviderKey), selected)) {
        return;
    }
    emit selectedRuntimeProviderChanged();
}

QString AppSettings::selectedCloudProvider() const {
    if (!store_) {
        return QStringLiteral("openai");
    }
    const auto stored =
        store_->value(QString::fromLatin1(selectedCloudProviderKey), QStringLiteral("openai"))
            .trimmed()
            .toLower();
    if (stored == QStringLiteral("claude") || stored == QStringLiteral("gemini") ||
        stored == QStringLiteral("deepseek") || stored == QStringLiteral("groq") ||
        stored == QStringLiteral("mistral")) {
        return stored;
    }
    return QStringLiteral("openai");
}

void AppSettings::setSelectedCloudProvider(const QString& providerId) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = providerId.trimmed().toLower();
    const auto selected =
        (normalized == QStringLiteral("claude") || normalized == QStringLiteral("gemini") ||
         normalized == QStringLiteral("deepseek") || normalized == QStringLiteral("groq") ||
         normalized == QStringLiteral("mistral"))
            ? normalized
            : QStringLiteral("openai");
    if (selected == selectedCloudProvider() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(selectedCloudProviderKey), selected)) {
        return;
    }
    emit selectedCloudProviderChanged();
}

QString AppSettings::selectedLocalModel() const {
    return selectedModelForProvider(QStringLiteral("ollama"));
}

void AppSettings::setSelectedLocalModel(const QString& model) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    setSelectedModelForProvider(QStringLiteral("ollama"), model);
}

QString AppSettings::selectedModelForProvider(const QString& providerId) const {
    if (!store_) {
        return {};
    }

    const auto normalizedProvider = providerId.trimmed().toLower();
    if (normalizedProvider.isEmpty()) {
        return {};
    }

    const auto providerKey =
        QString::fromLatin1(selectedProviderModelKeyPrefix) + normalizedProvider;
    const auto fallback = normalizedProvider == QStringLiteral("ollama")
                              ? store_->value(QString::fromLatin1(selectedLocalModelKey), {})
                              : QString();
    return store_->value(providerKey, fallback).trimmed();
}

void AppSettings::setSelectedModelForProvider(const QString& providerId, const QString& model) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_) {
        return;
    }

    const auto normalizedProvider = providerId.trimmed().toLower();
    if (normalizedProvider.isEmpty()) {
        return;
    }

    const auto normalized = model.trimmed();
    if (normalized == selectedModelForProvider(normalizedProvider)) {
        return;
    }

    QMap<QString, QString> changes{
        {QString::fromLatin1(selectedProviderModelKeyPrefix) + normalizedProvider, normalized}};
    if (normalizedProvider == QStringLiteral("ollama")) {
        changes.insert(QString::fromLatin1(selectedLocalModelKey), normalized);
    }
    if (!persistValues(changes)) {
        return;
    }
    emit selectedLocalModelChanged();
}

ModelCapabilities AppSettings::modelCapabilitiesOverride(const QString& providerId,
                                                          const QString& modelId) const {
    ModelCapabilities capabilities;
    if (!store_ || providerId.trimmed().isEmpty() || modelId.trimmed().isEmpty())
        return capabilities;
    const auto key = QStringLiteral("modelCapabilities.%1.%2")
                         .arg(providerId.trimmed().toLower(), modelId.trimmed());
    const auto value = store_->value(key, QString{});
    const auto document = QJsonDocument::fromJson(value.toUtf8());
    if (!document.isObject())
        return capabilities;
    auto support = [](const QJsonValue& capabilityValue) {
        if (capabilityValue.toString() == QLatin1String("supported"))
            return CapabilitySupport::Supported;
        if (capabilityValue.toString() == QLatin1String("unsupported"))
            return CapabilitySupport::Unsupported;
        return CapabilitySupport::Unknown;
    };
    const auto object = document.object();
    capabilities.streaming = support(object.value(QStringLiteral("streaming")));
    capabilities.nativeToolCalling = support(object.value(QStringLiteral("nativeToolCalling")));
    capabilities.structuredOutput = support(object.value(QStringLiteral("structuredOutput")));
    capabilities.visionInput = support(object.value(QStringLiteral("visionInput")));
    capabilities.audioInput = support(object.value(QStringLiteral("audioInput")));
    capabilities.audioOutput = support(object.value(QStringLiteral("audioOutput")));
    capabilities.combinedToolsAndStructuredOutput =
        support(object.value(QStringLiteral("combinedToolsAndStructuredOutput")));
    if (object.value(QStringLiteral("contextWindow")).isDouble()) {
        const int contextWindow = object.value(QStringLiteral("contextWindow")).toInt();
        if (contextWindow > 0)
            capabilities.contextWindow = contextWindow;
    }
    if (object.value(QStringLiteral("maxOutputTokens")).isDouble()) {
        const int maxOutputTokens = object.value(QStringLiteral("maxOutputTokens")).toInt();
        if (maxOutputTokens > 0)
            capabilities.maxOutputTokens = maxOutputTokens;
    }
    return capabilities;
}

void AppSettings::setModelCapabilitiesOverride(const QString& providerId, const QString& modelId,
                                                const ModelCapabilities& capabilities) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || providerId.trimmed().isEmpty() || modelId.trimmed().isEmpty())
        return;
    QJsonObject object;
    auto addSupport = [&object](const QString& key, CapabilitySupport value) {
        if (value == CapabilitySupport::Supported)
            object.insert(key, QStringLiteral("supported"));
        else if (value == CapabilitySupport::Unsupported)
            object.insert(key, QStringLiteral("unsupported"));
    };
    addSupport(QStringLiteral("streaming"), capabilities.streaming);
    addSupport(QStringLiteral("nativeToolCalling"), capabilities.nativeToolCalling);
    addSupport(QStringLiteral("structuredOutput"), capabilities.structuredOutput);
    addSupport(QStringLiteral("visionInput"), capabilities.visionInput);
    addSupport(QStringLiteral("audioInput"), capabilities.audioInput);
    addSupport(QStringLiteral("audioOutput"), capabilities.audioOutput);
    addSupport(QStringLiteral("combinedToolsAndStructuredOutput"),
               capabilities.combinedToolsAndStructuredOutput);
    if (capabilities.contextWindow && *capabilities.contextWindow > 0)
        object.insert(QStringLiteral("contextWindow"), *capabilities.contextWindow);
    if (capabilities.maxOutputTokens && *capabilities.maxOutputTokens > 0)
        object.insert(QStringLiteral("maxOutputTokens"), *capabilities.maxOutputTokens);
    const auto key = QStringLiteral("modelCapabilities.%1.%2")
                         .arg(providerId.trimmed().toLower(), modelId.trimmed());
    if (object.isEmpty()) {
        if (!removeValue(key)) {
            return;
        }
    } else {
        if (!persistValue(
                key, QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)))) {
            return;
        }
    }
}

QString AppSettings::selectedModelForRole(const QString& roleId) const {
    if (!store_) {
        return {};
    }

    const auto normalizedRole = roleId.trimmed().toLower();
    if (!isKnownModelRoleId(normalizedRole)) {
        return {};
    }

    return store_->value(QString::fromLatin1(selectedRoleModelKeyPrefix) + normalizedRole, {})
        .trimmed();
}

void AppSettings::setSelectedModelForRole(const QString& roleId, const QString& modelId) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_) {
        return;
    }

    const auto normalizedRole = roleId.trimmed().toLower();
    if (!isKnownModelRoleId(normalizedRole)) {
        return;
    }

    const auto normalizedModel = modelId.trimmed();
    if (normalizedModel == selectedModelForRole(normalizedRole)) {
        return;
    }

    if (!persistValue(QString::fromLatin1(selectedRoleModelKeyPrefix) + normalizedRole,
                      normalizedModel)) {
        return;
    }
    emit selectedModelRoleChanged();
}

bool AppSettings::localChatInferenceEnabled() const {
    return true;
}

void AppSettings::setLocalChatInferenceEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!enabled || !store_) {
        return;
    }

    if (store_->value(QString::fromLatin1(localChatInferenceEnabledKey), QStringLiteral("true")) ==
        QStringLiteral("true")) {
        return;
    }

    if (!persistValue(QString::fromLatin1(localChatInferenceEnabledKey), QStringLiteral("true"))) {
        return;
    }
    emit localChatInferenceEnabledChanged();
}

bool AppSettings::localInferenceStreamingEnabled() const {
    return true;
}

void AppSettings::setLocalInferenceStreamingEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!enabled || !store_) {
        return;
    }

    if (store_->value(QString::fromLatin1(localInferenceStreamingEnabledKey),
                      QStringLiteral("true")) == QStringLiteral("true")) {
        return;
    }

    if (!persistValue(QString::fromLatin1(localInferenceStreamingEnabledKey),
                      QStringLiteral("true"))) {
        return;
    }
    emit localInferenceStreamingEnabledChanged();
}

int AppSettings::localInferenceTimeoutMs() const {
    const auto fallback = QString::number(defaultLocalInferenceTimeoutMs);
    bool ok = false;
    const auto value =
        store_ ? store_->value(QString::fromLatin1(localInferenceTimeoutMsKey), fallback).toInt(&ok)
               : defaultLocalInferenceTimeoutMs;
    if (!ok) {
        return defaultLocalInferenceTimeoutMs;
    }
    return value <= 0 ? 0 : std::clamp(value, 1000, 300000);
}

void AppSettings::setLocalInferenceTimeoutMs(int timeoutMs) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = timeoutMs <= 0 ? 0 : std::clamp(timeoutMs, 1000, 300000);
    if (normalized == localInferenceTimeoutMs() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(localInferenceTimeoutMsKey),
                      QString::number(normalized))) {
        return;
    }
    emit localInferenceTimeoutMsChanged();
}

double AppSettings::localInferenceTemperature() const {
    const auto fallback = QString::number(defaultLocalInferenceTemperature);
    bool ok = false;
    const auto value =
        store_ ? store_->value(QString::fromLatin1(localInferenceTemperatureKey), fallback)
                     .toDouble(&ok)
               : defaultLocalInferenceTemperature;
    if (!ok) {
        return defaultLocalInferenceTemperature;
    }
    return std::clamp(value, 0.0, 2.0);
}

void AppSettings::setLocalInferenceTemperature(double temperature) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = std::clamp(temperature, 0.0, 2.0);
    if (qFuzzyCompare(normalized, localInferenceTemperature()) || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(localInferenceTemperatureKey),
                      QString::number(normalized, 'f', 2))) {
        return;
    }
    emit localInferenceTemperatureChanged();
}

double AppSettings::localInferenceTopP() const {
    const auto fallback = QString::number(defaultLocalInferenceTopP);
    bool ok = false;
    const auto value =
        store_ ? store_->value(QString::fromLatin1(localInferenceTopPKey), fallback).toDouble(&ok)
               : defaultLocalInferenceTopP;
    if (!ok) {
        return defaultLocalInferenceTopP;
    }
    return std::clamp(value, 0.0, 1.0);
}

void AppSettings::setLocalInferenceTopP(double topP) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = std::clamp(topP, 0.0, 1.0);
    if (qFuzzyCompare(normalized, localInferenceTopP()) || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(localInferenceTopPKey),
                      QString::number(normalized, 'f', 2))) {
        return;
    }
    emit localInferenceTopPChanged();
}

int AppSettings::localInferenceMaxTokens() const {
    const auto fallback = QString::number(defaultLocalInferenceMaxTokens);
    bool ok = false;
    const auto value =
        store_ ? store_->value(QString::fromLatin1(localInferenceMaxTokensKey), fallback).toInt(&ok)
               : defaultLocalInferenceMaxTokens;
    if (!ok) {
        return defaultLocalInferenceMaxTokens;
    }
    return std::clamp(value, 1, 16384);
}

void AppSettings::setLocalInferenceMaxTokens(int maxTokens) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = std::clamp(maxTokens, 1, 16384);
    if (normalized == localInferenceMaxTokens() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(localInferenceMaxTokensKey),
                      QString::number(normalized))) {
        return;
    }
    emit localInferenceMaxTokensChanged();
}

bool AppSettings::promptContextInjectionEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(promptContextInjectionEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setPromptContextInjectionEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == promptContextInjectionEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(promptContextInjectionEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit promptContextInjectionEnabledChanged();
}

bool AppSettings::semanticPromptInclusionEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(semanticPromptInclusionEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setSemanticPromptInclusionEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == semanticPromptInclusionEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(semanticPromptInclusionEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit semanticPromptInclusionEnabledChanged();
}

bool AppSettings::contextExplainabilityVisible() const {
    return store_ ? store_->value(QString::fromLatin1(contextExplainabilityVisibleKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setContextExplainabilityVisible(bool visible) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (visible == contextExplainabilityVisible() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(contextExplainabilityVisibleKey),
                      visible ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit contextExplainabilityVisibleChanged();
}

bool AppSettings::quickPanelStartAtLogin() const {
    return store_ && store_->value(QStringLiteral("quickPanelStartAtLogin"),
                                   QStringLiteral("false")) == QLatin1String("true");
}
void AppSettings::setQuickPanelStartAtLogin(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || enabled == quickPanelStartAtLogin())
        return;
    if (!persistValue(QStringLiteral("quickPanelStartAtLogin"),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit quickPanelStartAtLoginChanged();
}
QString AppSettings::quickPanelShortcut() const {
    return store_ ? store_->value(QStringLiteral("quickPanelShortcut"),
                                  QStringLiteral("Disabled"))
                  : QStringLiteral("Disabled");
}
void AppSettings::setQuickPanelShortcut(const QString& sequence) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || sequence == quickPanelShortcut() || sequence.size() > 64)
        return;
    if (!persistValue(QStringLiteral("quickPanelShortcut"), sequence)) {
        return;
    }
    emit quickPanelShortcutChanged();
}
bool AppSettings::companionEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(companionEnabledKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setCompanionEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == companionEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(companionEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit companionEnabledChanged();
}

bool AppSettings::developerModeEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(developerModeEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setDeveloperModeEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == developerModeEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(developerModeEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit developerModeEnabledChanged();
}

bool AppSettings::agentAutonomousMode() const {
    return store_ ? store_->value(QString::fromLatin1(agentAutonomousModeKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setAgentAutonomousMode(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == agentAutonomousMode() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(agentAutonomousModeKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit agentAutonomousModeChanged();
}

namespace {

QString findPiperBinaryCandidate() {
#if defined(Q_OS_WIN)
    QString exe = QStandardPaths::findExecutable(QStringLiteral("piper.exe"));
#else
    QString exe = QStandardPaths::findExecutable(QStringLiteral("piper"));
#endif
    if (!exe.isEmpty()) {
        return exe;
    }
#if defined(Q_OS_WIN)
    const QStringList candidates{
        QStringLiteral("C:\\Program Files\\Piper\\piper.exe"),
        QStringLiteral("C:\\Program Files (x86)\\Piper\\piper.exe"),
        QStringLiteral("C:\\tools\\piper\\piper.exe"),
        QDir::homePath() + QStringLiteral("\\piper\\piper.exe"),
    };
#else
    const QStringList candidates{
        QStringLiteral("/opt/homebrew/bin/piper"),
        QStringLiteral("/usr/local/bin/piper"),
        QStringLiteral("/usr/bin/piper"),
        QStringLiteral("/bin/piper"),
        QDir::homePath() + QStringLiteral("/.local/bin/piper"),
        QDir::homePath() + QStringLiteral("/piper/piper"),
    };
#endif
    for (const auto& c : candidates) {
        if (QFile::exists(c)) {
            return c;
        }
    }
    return {};
}

QString findWhisperBinaryCandidate() {
#if defined(Q_OS_WIN)
    QString exe = QStandardPaths::findExecutable(QStringLiteral("whisper.exe"));
    if (exe.isEmpty()) {
        exe = QStandardPaths::findExecutable(QStringLiteral("whisper-cli.exe"));
    }
    if (exe.isEmpty()) {
        exe = QStandardPaths::findExecutable(QStringLiteral("whisper-cpp.exe"));
    }
    if (exe.isEmpty()) {
        exe = QStandardPaths::findExecutable(QStringLiteral("main.exe"));
    }
#else
    QString exe = QStandardPaths::findExecutable(QStringLiteral("whisper"));
    if (exe.isEmpty()) {
        exe = QStandardPaths::findExecutable(QStringLiteral("whisper-cli"));
    }
    if (exe.isEmpty()) {
        exe = QStandardPaths::findExecutable(QStringLiteral("whisper-cpp"));
    }
    if (exe.isEmpty()) {
        exe = QStandardPaths::findExecutable(QStringLiteral("main"));
    }
#endif
    if (!exe.isEmpty()) {
        return exe;
    }
#if defined(Q_OS_WIN)
    const QStringList candidates{
        QStringLiteral("C:\\Program Files\\Whisper\\whisper.exe"),
        QStringLiteral("C:\\Program Files\\Whisper\\whisper-cli.exe"),
        QStringLiteral("C:\\Program Files\\Whisper\\whisper-cpp.exe"),
        QStringLiteral("C:\\Program Files (x86)\\Whisper\\whisper.exe"),
        QStringLiteral("C:\\Program Files (x86)\\Whisper\\whisper-cli.exe"),
        QStringLiteral("C:\\tools\\whisper\\whisper.exe"),
        QStringLiteral("C:\\tools\\whisper\\whisper-cli.exe"),
        QStringLiteral("C:\\tools\\whisper\\whisper-cpp.exe"),
    };
#else
    const QStringList candidates{
        QStringLiteral("/opt/homebrew/bin/whisper"),
        QStringLiteral("/usr/local/bin/whisper"),
        QStringLiteral("/usr/bin/whisper"),
        QStringLiteral("/bin/whisper"),
        QStringLiteral("/opt/homebrew/bin/whisper-cli"),
        QStringLiteral("/usr/local/bin/whisper-cli"),
        QStringLiteral("/usr/bin/whisper-cli"),
        QStringLiteral("/opt/homebrew/bin/whisper-cpp"),
        QStringLiteral("/usr/local/bin/whisper-cpp"),
        QStringLiteral("/usr/bin/whisper-cpp"),
        QDir::homePath() + QStringLiteral("/.local/bin/whisper"),
        QDir::homePath() + QStringLiteral("/.local/bin/whisper-cli"),
        QDir::homePath() + QStringLiteral("/.local/bin/whisper-cpp"),
    };
#endif
    for (const auto& c : candidates) {
        if (QFile::exists(c)) {
            return c;
        }
    }
    return {};
}

QString findModelInDirectories(const QStringList& dirs, const QStringList& filters) {
    for (const auto& dirPath : dirs) {
        QDir dir(dirPath);
        if (!dir.exists()) {
            continue;
        }
        for (const auto& filter : filters) {
            const QStringList files = dir.entryList({filter}, QDir::Files, QDir::Name);
            if (!files.isEmpty()) {
                return dir.filePath(files.first());
            }
        }
    }
    return {};
}

QString findModelInExternalDirectories(const QStringList& roots, const QStringList& filters) {
    for (const auto& root : roots) {
        QDirIterator iterator(root, filters, QDir::Files, QDirIterator::Subdirectories);
        if (iterator.hasNext()) {
            return iterator.next();
        }
    }
    return {};
}

QString findPiperModelCandidate() {
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString home = QDir::homePath();
    const QStringList dirs{
        appData + QStringLiteral("/models/piper"),
        appData + QStringLiteral("/piper"),
        home + QStringLiteral("/.local/share/piper"),
        home + QStringLiteral("/.cache/piper"),
        home + QStringLiteral("/models/piper"),
        home + QStringLiteral("/piper/models"),
        home + QStringLiteral("/piper"),
        QStringLiteral("/usr/share/piper"),
        QStringLiteral("/usr/share/sound/piper"),
    };
    return findModelInDirectories(dirs, {QStringLiteral("*.onnx")});
}

QString findWhisperModelCandidate() {
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString home = QDir::homePath();
    const QStringList dirs{
        appData + QStringLiteral("/models/whisper"),
        appData + QStringLiteral("/whisper"),
        home + QStringLiteral("/.local/share/whisper"),
        home + QStringLiteral("/.cache/whisper"),
        home + QStringLiteral("/models/whisper"),
        home + QStringLiteral("/whisper/models"),
        home + QStringLiteral("/whisper"),
        QStringLiteral("/usr/share/whisper"),
    };
    return findModelInDirectories(dirs, {QStringLiteral("*.bin"), QStringLiteral("*.gguf")});
}

QString findKokoroModelCandidate() {
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString home = QDir::homePath();
    const QStringList dirs{
        appData + QStringLiteral("/models/kokoro"),
        appData + QStringLiteral("/kokoro"),
        home + QStringLiteral("/.local/share/kokoro"),
        home + QStringLiteral("/.cache/kokoro"),
        home + QStringLiteral("/models/kokoro"),
        home + QStringLiteral("/.kokoro"),
        home + QStringLiteral("/kokoro"),
    };
    const QStringList filters{QStringLiteral("*.onnx"), QStringLiteral("*.pt"),
                              QStringLiteral("*.pth"), QStringLiteral("*.bin"),
                              QStringLiteral("*.safetensors")};
    const auto localCandidate = findModelInDirectories(dirs, filters);
    if (!localCandidate.isEmpty()) {
        return localCandidate;
    }

    // Large TTS models are often stored on mounted volumes outside the home directory.
    return findModelInExternalDirectories(
        {QStringLiteral("/Volumes"), QStringLiteral("/mnt"), QStringLiteral("/media")}, filters);
}

} // namespace

QString AppSettings::piperBinaryPath() const {
    return store_ ? store_->value(QString::fromLatin1(piperBinaryPathKey), {}).trimmed()
                  : QString();
}

void AppSettings::setPiperBinaryPath(const QString& path) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = path.trimmed();
    if (normalized == piperBinaryPath() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(piperBinaryPathKey), normalized)) {
        return;
    }
    emit piperBinaryPathChanged();
}

QString AppSettings::piperModelPath() const {
    return store_ ? store_->value(QString::fromLatin1(piperModelPathKey), {}).trimmed() : QString();
}

void AppSettings::setPiperModelPath(const QString& path) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = path.trimmed();
    if (normalized == piperModelPath() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(piperModelPathKey), normalized)) {
        return;
    }
    emit piperModelPathChanged();
}

QString AppSettings::selectedTtsEngine() const {
    const auto fallback = QString::fromLatin1(defaultSelectedTtsEngine);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(selectedTtsEngineKey), fallback).trimmed()
               : fallback;
    return (stored == QStringLiteral("Kokoro")) ? QStringLiteral("Kokoro")
                                                : QStringLiteral("Piper");
}

void AppSettings::setSelectedTtsEngine(const QString& engine) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = engine.trimmed();
    const auto value = (normalized == QStringLiteral("Kokoro")) ? QStringLiteral("Kokoro")
                                                                : QStringLiteral("Piper");
    if (value == selectedTtsEngine() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(selectedTtsEngineKey), value)) {
        return;
    }
    emit selectedTtsEngineChanged();
}

QString AppSettings::kokoroModelPath() const {
    return store_ ? store_->value(QString::fromLatin1(kokoroModelPathKey), {}).trimmed()
                  : QString();
}

void AppSettings::setKokoroModelPath(const QString& path) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = path.trimmed();
    if (normalized == kokoroModelPath() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(kokoroModelPathKey), normalized)) {
        return;
    }
    emit kokoroModelPathChanged();
}

QString AppSettings::kokoroVoice() const {
    const auto fallback = QString::fromLatin1(defaultKokoroVoice);
    return store_ ? store_->value(QString::fromLatin1(kokoroVoiceKey), fallback).trimmed()
                  : fallback;
}

void AppSettings::setKokoroVoice(const QString& voice) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = voice.trimmed();
    if (normalized == kokoroVoice() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(kokoroVoiceKey), normalized)) {
        return;
    }
    emit kokoroVoiceChanged();
}

QString AppSettings::voiceInputSource() const {
    return store_ ? store_->value(QStringLiteral("voiceInputSource"), QStringLiteral("local")) : QStringLiteral("local");
}
void AppSettings::setVoiceInputSource(const QString& source) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (store_ && (source == QLatin1String("local") || source == QLatin1String("system")))
        if (!persistValue(QStringLiteral("voiceInputSource"), source)) {
            return;
        }
}

QString AppSettings::whisperBinaryPath() const {
    return store_ ? store_->value(QString::fromLatin1(whisperBinaryPathKey), {}).trimmed()
                  : QString();
}

void AppSettings::setWhisperBinaryPath(const QString& path) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = path.trimmed();
    if (normalized == whisperBinaryPath() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(whisperBinaryPathKey), normalized)) {
        return;
    }
    emit whisperBinaryPathChanged();
}

QString AppSettings::whisperModelPath() const {
    return store_ ? store_->value(QString::fromLatin1(whisperModelPathKey), {}).trimmed()
                  : QString();
}

void AppSettings::setWhisperModelPath(const QString& path) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = path.trimmed();
    if (normalized == whisperModelPath() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(whisperModelPathKey), normalized)) {
        return;
    }
    emit whisperModelPathChanged();
}

QString AppSettings::autoDetectVoicePaths() {
    QStringList detected;

    const QString piperBin = findPiperBinaryCandidate();
    if (!piperBin.isEmpty()) {
        setPiperBinaryPath(piperBin);
        detected.append(QStringLiteral("Piper Binary: %1").arg(piperBin));
    }

    const QString piperMod = findPiperModelCandidate();
    if (!piperMod.isEmpty()) {
        setPiperModelPath(piperMod);
        detected.append(QStringLiteral("Piper Model: %1").arg(piperMod));
    }

    const QString whisperBin = findWhisperBinaryCandidate();
    if (!whisperBin.isEmpty()) {
        setWhisperBinaryPath(whisperBin);
        detected.append(QStringLiteral("Whisper Binary: %1").arg(whisperBin));
    }

    const QString whisperMod = findWhisperModelCandidate();
    if (!whisperMod.isEmpty()) {
        setWhisperModelPath(whisperMod);
        detected.append(QStringLiteral("Whisper Model: %1").arg(whisperMod));
    }

    const QString kokoroMod = findKokoroModelCandidate();
    if (!kokoroMod.isEmpty()) {
        setKokoroModelPath(kokoroMod);
        detected.append(QStringLiteral("Kokoro Model: %1").arg(kokoroMod));
    }

    if (detected.isEmpty()) {
        return QStringLiteral(
            "No local voice binaries or model files were detected automatically.");
    }
    return QStringLiteral("Auto-detected voice paths:\n- ") + detected.join(QStringLiteral("\n- "));
}

bool AppSettings::piperFileOutputExecutionEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(piperFileOutputExecutionEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setPiperFileOutputExecutionEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == piperFileOutputExecutionEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(piperFileOutputExecutionEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit piperFileOutputExecutionEnabledChanged();
}

bool AppSettings::whisperTranscriptionExecutionEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(whisperTranscriptionExecutionEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setWhisperTranscriptionExecutionEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == whisperTranscriptionExecutionEnabled() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(whisperTranscriptionExecutionEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit whisperTranscriptionExecutionEnabledChanged();
}

QString AppSettings::activeConversationId() const {
    return store_ ? store_->value(QString::fromLatin1(activeConversationIdKey), {}).trimmed()
                  : QString();
}

void AppSettings::setActiveConversationId(const QString& conversationId) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = conversationId.trimmed();
    if (normalized == activeConversationId() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(activeConversationIdKey), normalized)) {
        return;
    }
    emit activeConversationIdChanged();
}

QString AppSettings::selectedWorkspaceId() const {
    const auto fallback = QString::fromLatin1(defaultSelectedWorkspaceId);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(selectedWorkspaceIdKey), fallback) : fallback;
    return normalizedWorkspaceIdSetting(stored);
}

void AppSettings::setSelectedWorkspaceId(const QString& workspaceId) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedWorkspaceIdSetting(workspaceId);
    if (selected == selectedWorkspaceId() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(selectedWorkspaceIdKey), selected)) {
        return;
    }
    applyNetworkPolicy();
    emit selectedWorkspaceIdChanged();
}

QString AppSettings::defaultWorkspaceId() const {
    const auto fallback = QString::fromLatin1(defaultSelectedWorkspaceId);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(defaultWorkspaceIdKey), fallback) : fallback;
    return normalizedWorkspaceIdSetting(stored);
}

void AppSettings::setDefaultWorkspaceId(const QString& workspaceId) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedWorkspaceIdSetting(workspaceId);
    if (selected == defaultWorkspaceId() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(defaultWorkspaceIdKey), selected)) {
        return;
    }
    emit workspaceSettingsChanged();
}

QString AppSettings::workspaceCatalogJson() const {
    return store_ ? store_->value(QString::fromLatin1(workspaceCatalogJsonKey), {}) : QString();
}

void AppSettings::setWorkspaceCatalogJson(const QString& catalogJson) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }
    if (catalogJson == workspaceCatalogJson() || !store_) {
        return;
    }
    persistWorkspaceState(
        catalogJson, WorkspaceService{}.normalizedWorkspaceId(selectedWorkspaceId(), catalogJson));
}

QString AppSettings::workspaceProfilesJson() const {
    return store_ ? store_->value(QString::fromLatin1(workspaceProfilesJsonKey), {}) : QString();
}

void AppSettings::setWorkspaceProfilesJson(const QString& profilesJson) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (profilesJson == workspaceProfilesJson() || !store_) return;
    if (!profilesJson.isEmpty()) {
        const auto document = QJsonDocument::fromJson(profilesJson.toUtf8());
        if (!document.isObject() ||
            document.object().value(QStringLiteral("version")).toInt(-1) != 1 ||
            !document.object().value(QStringLiteral("workspaces")).isObject() ||
            !document.object().value(QStringLiteral("presets")).isObject()) return;
    }
    if (!persistValue(QString::fromLatin1(workspaceProfilesJsonKey), profilesJson)) {
        return;
    }
    applyNetworkPolicy();
    emit workspaceSettingsChanged();
}

bool AppSettings::localKnowledgeBaseEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(localKnowledgeBaseEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setLocalKnowledgeBaseEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == localKnowledgeBaseEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(localKnowledgeBaseEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit workspaceSettingsChanged();
}

bool AppSettings::retrievalExplainabilityEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(retrievalExplainabilityEnabledKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setRetrievalExplainabilityEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == retrievalExplainabilityEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(retrievalExplainabilityEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit workspaceSettingsChanged();
}

QString AppSettings::attachmentBehavior() const {
    const auto fallback = QString::fromLatin1(defaultAttachmentBehavior);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(attachmentBehaviorKey), fallback) : fallback;
    return normalizedAttachmentBehavior(stored);
}

void AppSettings::setAttachmentBehavior(const QString& behavior) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedAttachmentBehavior(behavior);
    if (selected == attachmentBehavior() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(attachmentBehaviorKey), selected)) {
        return;
    }
    emit workspaceSettingsChanged();
}

QString AppSettings::exportDefaultFormat() const {
    const auto fallback = QString::fromLatin1(defaultExportFormat);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(exportDefaultFormatKey), fallback) : fallback;
    return normalizedExportFormat(stored);
}

void AppSettings::setExportDefaultFormat(const QString& format) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedExportFormat(format);
    if (selected == exportDefaultFormat() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(exportDefaultFormatKey), selected)) {
        return;
    }
    emit workspaceSettingsChanged();
}

bool AppSettings::exportIncludeTimestamps() const {
    return store_ ? store_->value(QString::fromLatin1(exportIncludeTimestampsKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setExportIncludeTimestamps(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == exportIncludeTimestamps() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(exportIncludeTimestampsKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit workspaceSettingsChanged();
}

bool AppSettings::exportIncludeCitations() const {
    return store_ ? store_->value(QString::fromLatin1(exportIncludeCitationsKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setExportIncludeCitations(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == exportIncludeCitations() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(exportIncludeCitationsKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit workspaceSettingsChanged();
}

bool AppSettings::exportAnonymizeNames() const {
    return store_ ? store_->value(QString::fromLatin1(exportAnonymizeNamesKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setExportAnonymizeNames(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == exportAnonymizeNames() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(exportAnonymizeNamesKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit workspaceSettingsChanged();
}

bool AppSettings::exportIncludeModelMetadata() const {
    return store_ ? store_->value(QString::fromLatin1(exportIncludeModelMetadataKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setExportIncludeModelMetadata(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == exportIncludeModelMetadata() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(exportIncludeModelMetadataKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit workspaceSettingsChanged();
}

QString AppSettings::responseProfileInstructions() const {
    return store_ ? store_->value(QStringLiteral("responseProfileInstructions")).left(2000) : QString{};
}
void AppSettings::setResponseProfileInstructions(const QString& instructions) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || instructions.size() > 2000) return;
    const auto normalized = instructions.trimmed();
    if (normalized == responseProfileInstructions()) return;
    if (!persistValue(QStringLiteral("responseProfileInstructions"), normalized)) {
        return;
    }
    emit responseProfileInstructionsChanged();
}

QString AppSettings::selectedSkillProfile() const {
    const auto fallback = QString::fromLatin1(defaultSelectedSkillProfile);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(selectedSkillProfileKey), fallback) : fallback;
    const auto normalized = stored.trimmed().toLower();
    return isKnownSkillProfileId(normalized) ? normalized : fallback;
}

void AppSettings::setSelectedSkillProfile(const QString& profileId) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto normalized = profileId.trimmed().toLower();
    const auto selected = isKnownSkillProfileId(normalized)
                              ? normalized
                              : QString::fromLatin1(defaultSelectedSkillProfile);
    if (selected == selectedSkillProfile() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(selectedSkillProfileKey), selected)) {
        return;
    }
    emit selectedSkillProfileChanged();
}

QString AppSettings::defaultPermissionPolicyState() const {
    const auto fallback = QString::fromLatin1(defaultPermissionPolicyStateValue);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(defaultPermissionPolicyStateKey), fallback)
               : fallback;
    return normalizedPermissionPolicyState(stored);
}

void AppSettings::setDefaultPermissionPolicyState(const QString& state) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedPermissionPolicyState(state);
    if (selected == defaultPermissionPolicyState() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(defaultPermissionPolicyStateKey), selected)) {
        return;
    }
    emit defaultPermissionPolicyStateChanged();
}

QString AppSettings::updateCheckPolicy() const {
    const auto fallback = QString::fromLatin1(defaultUpdateCheckPolicy);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(updateCheckPolicyKey), fallback) : fallback;
    return normalizedUpdatePolicy(stored);
}

void AppSettings::setUpdateCheckPolicy(const QString& policy) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedUpdatePolicy(policy);
    if (selected == updateCheckPolicy() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(updateCheckPolicyKey), selected)) {
        return;
    }
    emit updateCheckPolicyChanged();
}

QString AppSettings::updateCheckUrl() const {
    const auto defaultUrl =
        QStringLiteral("https://api.github.com/repos/Sopwit/Sentinel/releases?per_page=1");
    return store_ ? store_->value(QString::fromLatin1(updateCheckUrlKey), defaultUrl) : defaultUrl;
}

void AppSettings::setUpdateCheckUrl(const QString& url) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (url == updateCheckUrl() || !store_)
        return;
    if (!persistValue(QString::fromLatin1(updateCheckUrlKey), url)) {
        return;
    }
    emit updateCheckUrlChanged();
}

QString AppSettings::notificationPolicy() const {
    const auto fallback = QString::fromLatin1(defaultNotificationPolicy);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(notificationPolicyKey), fallback) : fallback;
    return normalizedNotificationPolicy(stored);
}

void AppSettings::setNotificationPolicy(const QString& policy) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedNotificationPolicy(policy);
    if (selected == notificationPolicy() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(notificationPolicyKey), selected)) {
        return;
    }
    emit notificationPolicyChanged();
}

bool AppSettings::onboardingComplete() const {
    return store_ ? store_->value(QString::fromLatin1(onboardingCompleteKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setOnboardingComplete(bool complete) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (complete == onboardingComplete() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(onboardingCompleteKey),
                      complete ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit onboardingCompleteChanged();
}

QString AppSettings::onboardingFlowJson() const {
    return store_ ? store_->value(QString::fromLatin1(onboardingFlowJsonKey), {}) : QString();
}

void AppSettings::setOnboardingFlowJson(const QString& state) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || state == onboardingFlowJson()) return;
    if (!persistValue(QString::fromLatin1(onboardingFlowJsonKey), state)) {
        return;
    }
    emit onboardingUseCaseChanged();
}

QString AppSettings::onboardingUseCase() const {
    const auto fallback = QString::fromLatin1(defaultOnboardingUseCase);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(onboardingUseCaseKey), fallback) : fallback;
    return normalizedOnboardingUseCase(stored);
}

void AppSettings::setOnboardingUseCase(const QString& useCase) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedOnboardingUseCase(useCase);
    if (selected == onboardingUseCase() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(onboardingUseCaseKey), selected)) {
        return;
    }
    emit onboardingUseCaseChanged();
}

QString AppSettings::onboardingAiProvider() const {
    const auto fallback = QString::fromLatin1(defaultOnboardingAiProvider);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(onboardingAiProviderKey), fallback) : fallback;
    return normalizedOnboardingAiProvider(stored);
}

void AppSettings::setOnboardingAiProvider(const QString& provider) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedOnboardingAiProvider(provider);
    if (selected == onboardingAiProvider() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(onboardingAiProviderKey), selected)) {
        return;
    }
    emit onboardingUseCaseChanged();
}

QString AppSettings::selectedSystemMode() const {
    return store_
               ? store_->value(QString::fromLatin1(selectedSystemModeKey), QStringLiteral("Chat"))
               : QStringLiteral("Chat");
}

void AppSettings::setSelectedSystemMode(const QString& mode) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (mode == selectedSystemMode() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(selectedSystemModeKey), mode)) {
        return;
    }
    emit selectedSystemModeChanged();
}

QString AppSettings::recoveryDraftText() const {
    return store_ ? store_->value(QString::fromLatin1(recoveryDraftTextKey), {}) : QString();
}

void AppSettings::setRecoveryDraftText(const QString& text) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (text == recoveryDraftText() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(recoveryDraftTextKey), text)) {
        return;
    }
    emit recoveryDraftTextChanged();
}

bool AppSettings::reducedMotionEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(reducedMotionEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setReducedMotionEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == reducedMotionEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(reducedMotionEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit productExperienceChanged();
}

bool AppSettings::reducedTransparencyEnabled() const {
    return store_ && store_->value(QStringLiteral("reducedTransparencyEnabled"), QStringLiteral("false")) == QStringLiteral("true");
}

void AppSettings::setReducedTransparencyEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || enabled == reducedTransparencyEnabled()) return;
    if (!persistValue(QStringLiteral("reducedTransparencyEnabled"),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit productExperienceChanged();
}

bool AppSettings::highContrastEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(highContrastEnabledKey),
                                  QStringLiteral("false")) == QStringLiteral("true")
                  : false;
}

void AppSettings::setHighContrastEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == highContrastEnabled() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(highContrastEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit productExperienceChanged();
}

QString AppSettings::uiDensity() const {
    const auto fallback = QString::fromLatin1(defaultUiDensity);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(uiDensityKey), fallback) : fallback;
    return normalizedUiDensity(stored);
}

void AppSettings::setUiDensity(const QString& density) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedUiDensity(density);
    if (selected == uiDensity() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(uiDensityKey), selected)) {
        return;
    }
    emit productExperienceChanged();
}

bool AppSettings::notifyModelDownloads() const {
    return store_ ? store_->value(QString::fromLatin1(notifyModelDownloadsKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setNotifyModelDownloads(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == notifyModelDownloads() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(notifyModelDownloadsKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit productExperienceChanged();
}

bool AppSettings::notifyModelRemovals() const {
    return store_ ? store_->value(QString::fromLatin1(notifyModelRemovalsKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setNotifyModelRemovals(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == notifyModelRemovals() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(notifyModelRemovalsKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit productExperienceChanged();
}

bool AppSettings::notifyAgentResponses() const {
    return store_ ? store_->value(QString::fromLatin1(notifyAgentResponsesKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setNotifyAgentResponses(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == notifyAgentResponses() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(notifyAgentResponsesKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit productExperienceChanged();
}

bool AppSettings::notifySystemUpdates() const {
    return store_ ? store_->value(QString::fromLatin1(notifySystemUpdatesKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setNotifySystemUpdates(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == notifySystemUpdates() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(notifySystemUpdatesKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit productExperienceChanged();
}

QString AppSettings::notificationCenterJson() const {
    return store_ ? store_->value(QString::fromLatin1(notificationCenterJsonKey),
                                  QStringLiteral("{\"notifications\":[]}"))
                  : QStringLiteral("{\"notifications\":[]}");
}

void AppSettings::setNotificationCenterJson(const QString& json) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (json == notificationCenterJson() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(notificationCenterJsonKey), json)) {
        return;
    }
    emit productExperienceChanged();
}

QString AppSettings::updateWorkflowState() const {
    const auto fallback = QString::fromLatin1(defaultUpdateWorkflowState);
    const auto stored =
        store_ ? store_->value(QString::fromLatin1(updateWorkflowStateKey), fallback) : fallback;
    return normalizedUpdateWorkflowState(stored);
}

void AppSettings::setUpdateWorkflowState(const QString& state) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    const auto selected = normalizedUpdateWorkflowState(state);
    if (selected == updateWorkflowState() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(updateWorkflowStateKey), selected)) {
        return;
    }
    emit productExperienceChanged();
}

QString AppSettings::controlledAgentTasksJson() const {
    return store_ ? store_->value(QString::fromLatin1(controlledAgentTasksJsonKey),
                                  QStringLiteral("{\"tasks\":[]}"))
                  : QStringLiteral("{\"tasks\":[]}");
}

void AppSettings::setControlledAgentTasksJson(const QString& json) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (json == controlledAgentTasksJson() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(controlledAgentTasksJsonKey), json)) {
        return;
    }
    emit controlledAgentTasksChanged();
}

QString AppSettings::controlledAgentPermissionsJson() const {
    return store_ ? store_->value(QString::fromLatin1(controlledAgentPermissionsJsonKey),
                                  QStringLiteral("{\"permissions\":[]}"))
                  : QStringLiteral("{\"permissions\":[]}");
}

void AppSettings::setControlledAgentPermissionsJson(const QString& json) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (json == controlledAgentPermissionsJson() || !store_) {
        return;
    }

    if (!persistValue(QString::fromLatin1(controlledAgentPermissionsJsonKey), json)) {
        return;
    }
    emit controlledAgentTasksChanged();
}

QString AppSettings::openAiApiKey() const {
    return credential(QStringLiteral("openai"), QString::fromLatin1(openAiApiKeyKey));
}

void AppSettings::setOpenAiApiKey(const QString& key) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("openai"), QString::fromLatin1(openAiApiKeyKey), key))
        emit cloudApiKeysChanged();
}

QString AppSettings::claudeApiKey() const {
    return credential(QStringLiteral("claude"), QString::fromLatin1(claudeApiKeyKey));
}

void AppSettings::setClaudeApiKey(const QString& key) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("claude"), QString::fromLatin1(claudeApiKeyKey), key))
        emit cloudApiKeysChanged();
}

QString AppSettings::geminiApiKey() const {
    return credential(QStringLiteral("gemini"), QString::fromLatin1(geminiApiKeyKey));
}

void AppSettings::setGeminiApiKey(const QString& key) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("gemini"), QString::fromLatin1(geminiApiKeyKey), key))
        emit cloudApiKeysChanged();
}

QString AppSettings::deepseekApiKey() const {
    return credential(QStringLiteral("deepseek"), QString::fromLatin1(deepseekApiKeyKey));
}

void AppSettings::setDeepseekApiKey(const QString& key) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("deepseek"), QString::fromLatin1(deepseekApiKeyKey), key))
        emit cloudApiKeysChanged();
}

QString AppSettings::groqApiKey() const {
    return credential(QStringLiteral("groq"), QString::fromLatin1(groqApiKeyKey));
}

void AppSettings::setGroqApiKey(const QString& key) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("groq"), QString::fromLatin1(groqApiKeyKey), key))
        emit cloudApiKeysChanged();
}

QString AppSettings::mistralApiKey() const {
    return credential(QStringLiteral("mistral"), QString::fromLatin1(mistralApiKeyKey));
}

void AppSettings::setMistralApiKey(const QString& key) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("mistral"), QString::fromLatin1(mistralApiKeyKey), key))
        emit cloudApiKeysChanged();
}

// ── Proxy ─────────────────────────────────────────────────────────────────

bool AppSettings::proxyEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(proxyEnabledKey), QStringLiteral("false")) ==
                        QStringLiteral("true")
                  : false;
}

void AppSettings::setProxyEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_)
        return;
    if (!persistValue(QString::fromLatin1(proxyEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit proxySettingsChanged();
}

QString AppSettings::proxyType() const {
    const auto value = store_ ? store_->value(QString::fromLatin1(proxyTypeKey)) : QString();
    return value == QLatin1String("socks5") ? value : QStringLiteral("http");
}

void AppSettings::setProxyType(const QString& type) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || (type != QLatin1String("http") && type != QLatin1String("socks5")))
        return;
    if (!persistValue(QString::fromLatin1(proxyTypeKey), type)) {
        return;
    }
    emit proxySettingsChanged();
}

QString AppSettings::proxyHost() const {
    const auto host = store_ ? store_->value(QString::fromLatin1(proxyHostKey)) : QString();
    return host.contains(QLatin1String("://")) || host.contains(QLatin1Char('@')) ||
           host.contains(QLatin1Char('/')) || host.contains(QLatin1Char('?')) ||
           host.contains(QLatin1Char('#')) ? QString{} : host;
}

void AppSettings::setProxyHost(const QString& host) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || host.size() > 255 || host.contains(QLatin1String("://")) ||
        host.contains(QLatin1Char('@')) || host.contains(QLatin1Char('/')) ||
        host.contains(QLatin1Char('?')) || host.contains(QLatin1Char('#')))
        return;
    if (!persistValue(QString::fromLatin1(proxyHostKey), host)) {
        return;
    }
    emit proxySettingsChanged();
}

quint16 AppSettings::proxyPort() const {
    if (!store_)
        return 8080;
    bool ok = false;
    quint16 val = static_cast<quint16>(
        store_->value(QString::fromLatin1(proxyPortKey), QStringLiteral("8080")).toUShort(&ok));
    return ok ? val : 8080;
}

void AppSettings::setProxyPort(quint16 port) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_)
        return;
    if (!persistValue(QString::fromLatin1(proxyPortKey), QString::number(port))) {
        return;
    }
    emit proxySettingsChanged();
}

QString AppSettings::proxyUser() const {
    return credential(QStringLiteral("proxy-user"), QString::fromLatin1(proxyUserKey));
}

void AppSettings::setProxyUser(const QString& user) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("proxy-user"), QString::fromLatin1(proxyUserKey), user))
        emit proxySettingsChanged();
}

QString AppSettings::proxyPassword() const {
    return credential(QStringLiteral("proxy-password"), QString::fromLatin1(proxyPasswordKey));
}

void AppSettings::setProxyPassword(const QString& password) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (setCredential(QStringLiteral("proxy-password"), QString::fromLatin1(proxyPasswordKey), password))
        emit proxySettingsChanged();
}

QString AppSettings::networkMode() const {
    const auto value = store_ ? store_->value(QString::fromLatin1(networkModeKey)) : QString();
    return value == QLatin1String("Offline") || value == QLatin1String("LocalOnly")
        ? value : QStringLiteral("Online");
}

void AppSettings::setNetworkMode(const QString& mode) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (!store_ || (mode != QLatin1String("Online") && mode != QLatin1String("Offline") &&
                    mode != QLatin1String("LocalOnly"))) return;
    if (mode != networkMode()) {
        if (!persistValue(QString::fromLatin1(networkModeKey), mode)) {
            return;
        }
    }
    applyNetworkPolicy();
    emit networkModeChanged();
}

bool AppSettings::soundEffectsEnabled() const {
    return store_ ? store_->value(QString::fromLatin1(soundEffectsEnabledKey),
                                  QStringLiteral("true")) == QStringLiteral("true")
                  : true;
}

void AppSettings::setSoundEffectsEnabled(bool enabled) {
    if (QThread::currentThread() != thread()) {
        wrongThreadWrite_ = true;
        return;
    }

    if (enabled == soundEffectsEnabled() || !store_) {
        return;
    }
    if (!persistValue(QString::fromLatin1(soundEffectsEnabledKey),
                      enabled ? QStringLiteral("true") : QStringLiteral("false"))) {
        return;
    }
    emit soundEffectsEnabledChanged();
}

} // namespace sentinel::core
