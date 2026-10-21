// SPDX-License-Identifier: GPL-3.0-or-later
#include "DaemonDesktopSettings.h"
#include "sentinel/core/extension/ExtensionService.h"
namespace sentinel::daemon {
static core::SettingsService
composedSettingsService(core::AppSettings& settings,
                        const core::ApplicationController& controller) {
    return core::SettingsService(settings, controller.modelService(), controller.extensionService(),
                                 controller.audioSession(), controller.permissionService(),
                                 controller.conversationStore(), controller.memoryStore(),
                                 controller.mutableAgentRunStore(), controller.chatHistoryStore(),
                                 controller.modelOperations());
}

QVariantList DaemonDesktopSettings::productSettings() const {
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
QStringList DaemonDesktopSettings::productSettingsSections() const {
    return composedSettingsService(settings_, controller_).sectionIds();
}
QVariantMap DaemonDesktopSettings::setProductSetting(const QString& id, const QVariant& value) {
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
QVariantMap DaemonDesktopSettings::resetProductSetting(const QString& id) {
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
QVariantList DaemonDesktopSettings::resetProductSettingsSection(int section) {
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
QVariantMap DaemonDesktopSettings::clearWorkspaceSettingOverride(const QString& workspaceId,
                                                                 const QString& key) {
    const auto result =
        composedSettingsService(settings_, controller_).clearWorkspaceOverride(workspaceId, key);
    return {{QStringLiteral("accepted"), result.accepted},
            {QStringLiteral("code"), result.code},
            {QStringLiteral("field"), result.field}};
}
QVariantMap DaemonDesktopSettings::providerSettingsState(const QString& providerId) const {
    return composedSettingsService(settings_, controller_).providerState(providerId).toVariantMap();
}
QVariantMap DaemonDesktopSettings::extensionSettingsState(const QString& extensionId) const {
    return composedSettingsService(settings_, controller_)
        .extensionState(extensionId)
        .toVariantMap();
}
QVariantMap DaemonDesktopSettings::performExtensionSettingsAction(const QString& extensionId,
                                                                  int action) {
    const auto result =
        composedSettingsService(settings_, controller_).performExtensionAction(extensionId, action);
    return {{QStringLiteral("accepted"), result.accepted},
            {QStringLiteral("code"), result.code},
            {QStringLiteral("field"), result.field}};
}
QVariantMap DaemonDesktopSettings::speechSettingsState() const {
    return composedSettingsService(settings_, controller_).speechState().toVariantMap();
}
QVariantMap DaemonDesktopSettings::securitySettingsState() const {
    auto state = composedSettingsService(settings_, controller_).securityState().toVariantMap();
    state.insert(QStringLiteral("latestSandboxStatus"), controller_.latestSandboxStatus());
    state.insert(QStringLiteral("latestSandboxSummary"), controller_.latestSandboxSummary());
    return state;
}
QVariantMap DaemonDesktopSettings::productPrivacyState() const {
    return composedSettingsService(settings_, controller_).privacyState().toVariantMap();
}
QVariantMap DaemonDesktopSettings::productRecoveryState() const {
    return composedSettingsService(settings_, controller_).recoveryState().toVariantMap();
}
QVariantMap DaemonDesktopSettings::productBackupAvailability() const {
    return composedSettingsService(settings_, controller_).backupAvailability().toVariantMap();
}
QVariantMap DaemonDesktopSettings::exportProductBackup(const QStringList& domains) const {
    const auto result = composedSettingsService(settings_, controller_).exportBackupJson(domains);
    return {{QStringLiteral("succeeded"), result.succeeded()},
            {QStringLiteral("error"), static_cast<int>(result.error)},
            {QStringLiteral("data"), result.data},
            {QStringLiteral("detail"), result.detail}};
}
QVariantMap DaemonDesktopSettings::importProductBackup(const QByteArray& data,
                                                       const QStringList& domains, bool replace) {
    const auto result = composedSettingsService(settings_, controller_)
                            .importBackupJson(data, domains,
                                              replace ? core::ImportMode::ReplaceSelectedDomains
                                                      : core::ImportMode::Merge);
    return {{QStringLiteral("succeeded"), result.succeeded()},
            {QStringLiteral("error"), static_cast<int>(result.error)},
            {QStringLiteral("detail"), result.detail}};
}
QVariantMap DaemonDesktopSettings::clearProductData(const QString& domain) {
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
QVariantMap DaemonDesktopSettings::runProductMaintenance() {
    auto service = composedSettingsService(settings_, controller_);
    return service.runRetentionMaintenance().toVariantMap();
}
QVariantMap DaemonDesktopSettings::resolveInterruptedModelOperation(const QString& operationId) {
    const auto result = composedSettingsService(settings_, controller_)
                            .resolveInterruptedModelOperation(operationId);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}
QVariantMap DaemonDesktopSettings::clearProductCredential(const QString& id) {
    const auto result = composedSettingsService(settings_, controller_).clearCredential(id);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}
QVariantMap DaemonDesktopSettings::setProductProviderCredential(const QString& providerId,
                                                                const QString& value) {
    const auto result =
        composedSettingsService(settings_, controller_).setProviderCredential(providerId, value);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}
QVariantMap DaemonDesktopSettings::setProductPluginCredential(const QString& pluginId,
                                                              const QString& credentialId,
                                                              const QString& value) {
    const auto result = composedSettingsService(settings_, controller_)
                            .setPluginCredential(pluginId, credentialId, value);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}
QVariantMap DaemonDesktopSettings::clearProductPluginCredential(const QString& pluginId,
                                                                const QString& credentialId) {
    const auto result = composedSettingsService(settings_, controller_)
                            .clearPluginCredential(pluginId, credentialId);
    return {{QStringLiteral("accepted"), result.accepted}, {QStringLiteral("code"), result.code}};
}
QJsonObject DaemonDesktopSettings::dispatch(const QString& name, const QJsonArray& args) {
    if (name == "setProviderModelPreference") {
        const auto provider = args.at(0).toString();
        if (!controller_.modelService()->isKnownProvider(provider)) {
            return {{"accepted", false}, {"code", "UnknownProvider"}};
        }
        settings_.clearStorageWriteError();
        settings_.setSelectedModelForProvider(provider, args.at(1).toString());
        const auto error = settings_.storageErrorCode();
        return {{"accepted", error.isEmpty()}, {"code", error}};
    }
    if (name == "persistWorkspaceState") {
        const bool accepted =
            settings_.persistWorkspaceState(args.at(0).toString(), args.at(1).toString());
        return {{"accepted", accepted}, {"code", settings_.storageErrorCode()}};
    }
    if (name == "extensionsHubState") {
        const auto* service = controller_.extensionService();
        return service ? service->managementSnapshot()
                       : QJsonObject{{"code", "ExtensionServiceUnavailable"}};
    }
    if (name == "productSettings")
        return {{"value", QJsonArray::fromVariantList(productSettings())}};
    if (name == "productSettingsSections")
        return {{"value", QJsonArray::fromStringList(productSettingsSections())}};
    if (name == "setProductSetting")
        return QJsonObject::fromVariantMap(
            setProductSetting(args.at(0).toString(), args.at(1).toVariant()));
    if (name == "resetProductSetting")
        return QJsonObject::fromVariantMap(resetProductSetting(args.at(0).toString()));
    if (name == "resetProductSettingsSection")
        return {{"value",
                 QJsonArray::fromVariantList(resetProductSettingsSection(args.at(0).toInt()))}};
    if (name == "clearWorkspaceSettingOverride")
        return QJsonObject::fromVariantMap(
            clearWorkspaceSettingOverride(args.at(0).toString(), args.at(1).toString()));
    if (name == "providerSettingsState")
        return QJsonObject::fromVariantMap(providerSettingsState(args.at(0).toString()));
    if (name == "extensionSettingsState")
        return QJsonObject::fromVariantMap(extensionSettingsState(args.at(0).toString()));
    if (name == "performExtensionSettingsAction")
        return QJsonObject::fromVariantMap(
            performExtensionSettingsAction(args.at(0).toString(), args.at(1).toInt()));
    if (name == "speechSettingsState")
        return QJsonObject::fromVariantMap(speechSettingsState());
    if (name == "securitySettingsState")
        return QJsonObject::fromVariantMap(securitySettingsState());
    if (name == "productPrivacyState")
        return QJsonObject::fromVariantMap(productPrivacyState());
    if (name == "productRecoveryState")
        return QJsonObject::fromVariantMap(productRecoveryState());
    if (name == "productBackupAvailability")
        return QJsonObject::fromVariantMap(productBackupAvailability());
    if (name == "exportProductBackup") {
        auto result = exportProductBackup(args.at(0).toVariant().toStringList());
        result["data"] = result.value("data").toByteArray().toBase64();
        return QJsonObject::fromVariantMap(result);
    }
    if (name == "importProductBackup")
        return QJsonObject::fromVariantMap(
            importProductBackup(QByteArray::fromBase64(args.at(0).toString().toLatin1()),
                                args.at(1).toVariant().toStringList(), args.at(2).toBool()));
    if (name == "clearProductData")
        return QJsonObject::fromVariantMap(clearProductData(args.at(0).toString()));
    if (name == "runProductMaintenance")
        return QJsonObject::fromVariantMap(runProductMaintenance());
    if (name == "resolveInterruptedModelOperation")
        return QJsonObject::fromVariantMap(resolveInterruptedModelOperation(args.at(0).toString()));
    if (name == "clearProductCredential")
        return QJsonObject::fromVariantMap(clearProductCredential(args.at(0).toString()));
    if (name == "setProductProviderCredential")
        return QJsonObject::fromVariantMap(
            setProductProviderCredential(args.at(0).toString(), args.at(1).toString()));
    if (name == "setProductPluginCredential")
        return QJsonObject::fromVariantMap(setProductPluginCredential(
            args.at(0).toString(), args.at(1).toString(), args.at(2).toString()));
    if (name == "clearProductPluginCredential")
        return QJsonObject::fromVariantMap(
            clearProductPluginCredential(args.at(0).toString(), args.at(1).toString()));
    return {{"accepted", false}, {"code", "UnknownSettingsAction"}};
}
} // namespace sentinel::daemon
