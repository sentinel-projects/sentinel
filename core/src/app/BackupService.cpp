// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/BackupService.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/RecoveryService.h"
#include "sentinel/core/chat/IConversationStore.h"
#include "sentinel/core/interfaces/IMemoryStore.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QHostAddress>
#include <QSet>
#include <QUrl>

namespace sentinel::core {
namespace {
constexpr int maxBytes = 16 * 1024 * 1024;
const QStringList supported{QStringLiteral("settings"), QStringLiteral("workspaceProfiles"),
                            QStringLiteral("extensions"), QStringLiteral("chat"),
                            QStringLiteral("memory")};

BackupResult failed(BackupError error, QString detail) {
    return {error, {}, std::move(detail)};
}

bool validDomains(const QStringList& domains) {
    QSet<QString> seen;
    for (const auto& domain : domains) {
        if (!supported.contains(domain) || seen.contains(domain)) return false;
        seen.insert(domain);
    }
    return !domains.isEmpty();
}

bool validProfiles(const QString& json) {
    if (json.size() > 1024 * 1024) return false;
    const auto doc = QJsonDocument::fromJson(json.toUtf8());
    return doc.isObject() && doc.object().value(QStringLiteral("version")).toInt(-1) == 1 &&
           doc.object().value(QStringLiteral("workspaces")).isObject() &&
           doc.object().value(QStringLiteral("presets")).isObject();
}

QString roleName(ChatRole role) {
    return role == ChatRole::User ? QStringLiteral("user")
         : role == ChatRole::Assistant ? QStringLiteral("assistant")
                                       : QStringLiteral("system");
}
} // namespace

BackupService::BackupService(AppSettings& settings, IConversationStore* conversations,
                             IMemoryStore* memory)
    : settings_(settings), conversations_(conversations), memory_(memory) {}

QJsonObject BackupService::availability() const {
    const bool settingsReady = settings_.storageErrorCode().isEmpty();
    const bool chatReady =
        conversations_ && conversations_->status() == ConversationStoreStatus::Ready;
    const bool memoryReady = memory_ && memory_->isAvailable() && !memory_->hasManagedMemory();
    // This is an inventory of capabilities, never evidence that a backup was verified.
    return {{QStringLiteral("settings"), settingsReady},
            {QStringLiteral("workspaceProfiles"), settingsReady},
            {QStringLiteral("extensions"), settingsReady},
            {QStringLiteral("chat"), chatReady},
            {QStringLiteral("memory"), memoryReady},
            {QStringLiteral("formatVersion"), 1},
            {QStringLiteral("maximumSelectedDomains"), 1},
            {QStringLiteral("coordinatedBackupAvailable"), false},
            {QStringLiteral("coordinatedRestoreAvailable"), false},
            {QStringLiteral("coordinatedState"), QStringLiteral("Blocked")},
            {QStringLiteral("coordinatedReason"),
             QStringLiteral("OwnerQuiescenceAndDurableRecoveryUnavailable")},
            {QStringLiteral("includedDomains"), QJsonArray{}},
            {QStringLiteral("excludedDomains"),
             QJsonArray{"settings", "workspaceProfiles", "extensions", "chat", "memory",
                        "chatHistory", "agentRuns", "permissionGrants", "credentials",
                        "ipcSessions", "localRag", "modelFiles", "temporaryData"}},
            {QStringLiteral("lastVerifiedCoordinatedBackup"), QJsonValue{}},
            {QStringLiteral("recoveryInspection"), QStringLiteral("Unavailable")},
            {QStringLiteral("domainContracts"),
             QJsonArray{
                 QJsonObject{{"domain", "settings"}, {"state", "SingleDomainPortableExport"}},
                 QJsonObject{{"domain", "workspaceProfiles"}, {"state", "ExplicitSensitiveExport"}},
                 QJsonObject{{"domain", "extensions"}, {"state", "FilteredPortableExport"}},
                 QJsonObject{{"domain", "chat"}, {"state", "PortableExportWithoutSessionIdentity"}},
                 QJsonObject{{"domain", "memory"}, {"state", "UseAuthorizedScopeArchive"}},
                 QJsonObject{{"domain", "permissionGrants"}, {"state", "Excluded"}},
                 QJsonObject{{"domain", "credentials"}, {"state", "Excluded"}},
                 QJsonObject{{"domain", "agentRuns"}, {"state", "Excluded"}},
                 QJsonObject{{"domain", "chatHistory"}, {"state", "Excluded"}},
                 QJsonObject{{"domain", "ipcSessions"}, {"state", "Excluded"}},
                 QJsonObject{{"domain", "localRag"}, {"state", "ReconstructableCache"}}}}};
}

BackupResult BackupService::exportJson(const QStringList& domains) const {
    if (!validDomains(domains)) return failed(BackupError::ExportFailure, QStringLiteral("Invalid domains"));
    if (domains.size() > 1) {
        return failed(
            BackupError::CoordinatedOperationBlocked,
            QStringLiteral(
                "Coordinated backup blocked: storage owners have no shared quiescence barrier"));
    }
    if (!settings_.storageErrorCode().isEmpty())
        return failed(BackupError::ExportFailure, settings_.storageErrorCode());
    QJsonObject payload;
    if (domains.contains(QStringLiteral("settings"))) {
        payload.insert(QStringLiteral("settings"), QJsonObject{
            {QStringLiteral("theme"), settings_.themeName()},
            {QStringLiteral("language"), settings_.appLanguage()},
            {QStringLiteral("networkMode"), settings_.networkMode()}});
    }
    if (domains.contains(QStringLiteral("workspaceProfiles"))) {
        const auto profiles = settings_.workspaceProfilesJson();
        if (!profiles.isEmpty() && !validProfiles(profiles))
            return failed(BackupError::ExportFailure, QStringLiteral("Workspace profiles are invalid"));
        payload.insert(QStringLiteral("workspaceProfiles"), profiles);
    }
    if (domains.contains(QStringLiteral("extensions"))) {
        const auto document = QJsonDocument::fromJson(settings_.mcpServersJson().toUtf8());
        const auto root = document.object();
        const auto servers = root.value(QStringLiteral("mcpServers")).isObject()
            ? root.value(QStringLiteral("mcpServers")).toObject() : root;
        QJsonObject portable;
        for (auto it = servers.constBegin(); it != servers.constEnd(); ++it) {
            const auto source = it.value().toObject();
            const QUrl url(source.value(QStringLiteral("url")).toString());
            QHostAddress address;
            const bool loopback = url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 ||
                (address.setAddress(url.host()) && address.isLoopback());
            if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
                !url.query().isEmpty() || !url.fragment().isEmpty() ||
                (url.scheme() != QLatin1String("https") &&
                 !(loopback && url.scheme() == QLatin1String("http")))) continue;
            QJsonObject headers;
            const auto rawHeaders = source.value(QStringLiteral("headers")).toObject();
            for (auto header = rawHeaders.constBegin(); header != rawHeaders.constEnd(); ++header)
                if (header.value().toString().startsWith(QLatin1String("secret://")))
                    headers.insert(header.key(), header.value());
            portable.insert(it.key(), QJsonObject{
                {QStringLiteral("url"), url.toString()},
                {QStringLiteral("enabled"), source.value(QStringLiteral("enabled")).toBool(true)},
                {QStringLiteral("headers"), headers}});
        }
        payload.insert(QStringLiteral("extensions"), portable);
    }
    if (domains.contains(QStringLiteral("chat"))) {
        if (!conversations_ || conversations_->status() != ConversationStoreStatus::Ready)
            return failed(BackupError::ExportFailure, QStringLiteral("Chat store unavailable"));
        const auto conversations = conversations_->listConversations();
        if (conversations.size() > 100)
            return failed(BackupError::ExportFailure, QStringLiteral("Chat export exceeds limit"));
        QJsonArray items;
        for (const auto& conversation : conversations) {
            const auto messages = conversations_->loadMessages(conversation.id);
            if (messages.size() > 1000)
                return failed(BackupError::ExportFailure, QStringLiteral("Chat export exceeds limit"));
            QJsonArray serialized;
            for (const auto& message : messages)
                serialized.append(QJsonObject{{QStringLiteral("role"), roleName(message.role)},
                    {QStringLiteral("content"), message.content},
                    {QStringLiteral("timestamp"), message.timestampUtc.toUTC().toString(Qt::ISODateWithMs)}});
            items.append(QJsonObject{{QStringLiteral("title"), conversation.title},
                                     {QStringLiteral("messages"), serialized}});
        }
        payload.insert(QStringLiteral("chat"), items);
    }
    if (domains.contains(QStringLiteral("memory"))) {
        if (!memory_ || !memory_->isAvailable())
            return failed(BackupError::ExportFailure, QStringLiteral("Memory store unavailable"));
        if (memory_->hasManagedMemory()) {
            return failed(
                BackupError::ExportFailure,
                QStringLiteral("Managed memory export requires a scope-preserving format; "
                               "legacy export refused"));
        }
        const auto entries = memory_->entries();
        if (entries.size() > 10000)
            return failed(BackupError::ExportFailure, QStringLiteral("Memory export exceeds limit"));
        QJsonArray items;
        for (const auto& [key, value] : entries)
            items.append(QJsonObject{{QStringLiteral("key"), key},
                                     {QStringLiteral("value"), value}});
        payload.insert(QStringLiteral("memory"), items);
    }
    const QJsonObject manifest{{QStringLiteral("formatVersion"), 1},
        {QStringLiteral("createdAtUtc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("includedDomains"), QJsonArray::fromStringList(domains)},
        {QStringLiteral("applicationSchema"), QJsonObject{
            {QStringLiteral("settings"), 1}, {QStringLiteral("workspaceProfiles"), 1}}},
        {QStringLiteral("credentialsExcluded"), true},
        {QStringLiteral("modelBinariesExcluded"), true},
        {QStringLiteral("temporaryDataExcluded"), true}};
    const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("manifest"), manifest},
                                                 {QStringLiteral("data"), payload}})
                           .toJson(QJsonDocument::Compact);
    if (bytes.size() > maxBytes)
        return failed(BackupError::ExportFailure, QStringLiteral("Export exceeds size limit"));
    return {BackupError::None, bytes, {}};
}

BackupResult BackupService::importJson(const QByteArray& bytes,
                                       const QStringList& selectedDomains, ImportMode mode) {
    if (!settings_.storageErrorCode().isEmpty())
        return failed(BackupError::StoreFailure, settings_.storageErrorCode());
    if (bytes.isEmpty() || bytes.size() > maxBytes || !validDomains(selectedDomains))
        return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid import size or domains"));
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failed(BackupError::ImportValidationFailure, QStringLiteral("Malformed backup"));
    const auto root = document.object();
    const auto manifest = root.value(QStringLiteral("manifest")).toObject();
    const auto data = root.value(QStringLiteral("data")).toObject();
    if (manifest.value(QStringLiteral("formatVersion")).toInt(-1) != 1)
        return failed(BackupError::UnsupportedBackupVersion, QStringLiteral("Unsupported backup format"));
    if (!manifest.value(QStringLiteral("credentialsExcluded")).toBool() ||
        !QDateTime::fromString(manifest.value(QStringLiteral("createdAtUtc")).toString(),
                               Qt::ISODateWithMs).isValid())
        return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid manifest"));
    QStringList included;
    for (const auto& value : manifest.value(QStringLiteral("includedDomains")).toArray()) {
        if (!value.isString()) return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid domain"));
        included.append(value.toString());
    }
    if (!validDomains(included))
        return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid domain list"));
    for (const auto& domain : selectedDomains)
        if (!included.contains(domain))
            return failed(BackupError::ImportValidationFailure, QStringLiteral("Domain absent from backup"));
    const auto schema = manifest.value(QStringLiteral("applicationSchema")).toObject();
    if (schema.value(QStringLiteral("settings")).toInt(-1) != 1 ||
        schema.value(QStringLiteral("workspaceProfiles")).toInt(-1) != 1)
        return failed(BackupError::UnsupportedBackupVersion, QStringLiteral("Unsupported application schema"));

    if (selectedDomains.size() > 1) {
        return failed(
            BackupError::CoordinatedOperationBlocked,
            QStringLiteral(
                "Coordinated restore blocked: durable multi-domain recovery is unavailable"));
    }

    // Validate every selected domain, including conflicts, before the first mutation.
    const auto settings = data.value(QStringLiteral("settings")).toObject();
    if (selectedDomains.contains(QStringLiteral("settings"))) {
        const auto theme = settings.value(QStringLiteral("theme"));
        const auto language = settings.value(QStringLiteral("language"));
        const auto network = settings.value(QStringLiteral("networkMode"));
        if (!theme.isString() || theme.toString().size() > 100 || !language.isString() ||
            !settings_.availableLanguages().contains(language.toString()) || !network.isString() ||
            !QStringList{QStringLiteral("Online"), QStringLiteral("Offline"),
                         QStringLiteral("LocalOnly")}.contains(network.toString()))
            return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid settings"));
        if (mode == ImportMode::Merge &&
            (theme.toString() != settings_.themeName() ||
             language.toString() != settings_.appLanguage() ||
             network.toString() != settings_.networkMode()))
            return failed(BackupError::ImportConflict, QStringLiteral("Settings differ"));
    }
    const auto profiles = data.value(QStringLiteral("workspaceProfiles"));
    if (selectedDomains.contains(QStringLiteral("workspaceProfiles"))) {
        if (!profiles.isString() || (!profiles.toString().isEmpty() &&
                                      !validProfiles(profiles.toString())))
            return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid workspace profiles"));
        if (mode == ImportMode::Merge && !settings_.workspaceProfilesJson().isEmpty() &&
            profiles.toString() != settings_.workspaceProfilesJson())
            return failed(BackupError::ImportConflict, QStringLiteral("Workspace profiles differ"));
    }
    const auto extensions = data.value(QStringLiteral("extensions")).toObject();
    if (selectedDomains.contains(QStringLiteral("extensions"))) {
        if (!data.value(QStringLiteral("extensions")).isObject() || extensions.size() > 50)
            return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid extensions"));
        for (auto it = extensions.constBegin(); it != extensions.constEnd(); ++it) {
            const auto entry = it.value().toObject();
            const QUrl url(entry.value(QStringLiteral("url")).toString());
            QHostAddress address;
            const bool loopback = url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0 ||
                (address.setAddress(url.host()) && address.isLoopback());
            if (it.key().isEmpty() || it.key().size() > 100 || !url.isValid() ||
                url.host().isEmpty() || !url.userInfo().isEmpty() ||
                !url.query().isEmpty() || !url.fragment().isEmpty() ||
                (url.scheme() != QLatin1String("https") &&
                 !(loopback && url.scheme() == QLatin1String("http"))) ||
                !entry.value(QStringLiteral("headers")).isObject())
                return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid extension endpoint"));
            const auto headers = entry.value(QStringLiteral("headers")).toObject();
            if (headers.size() > 20)
                return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid extension headers"));
            for (auto header = headers.constBegin(); header != headers.constEnd(); ++header)
                if (!header.value().isString() || header.key().size() > 100 ||
                    header.value().toString() != QStringLiteral("secret://mcp-") +
                        QString::fromLatin1(QCryptographicHash::hash(
                            (it.key() + QLatin1Char(':') + header.key()).toUtf8(),
                            QCryptographicHash::Sha256).toHex()))
                    return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid secret reference"));
        }
        if (mode == ImportMode::Merge && !settings_.mcpServersJson().isEmpty())
            return failed(BackupError::ImportConflict, QStringLiteral("Extension configuration exists"));
    }
    const auto memories = data.value(QStringLiteral("memory")).toArray();
    if (selectedDomains.contains(QStringLiteral("memory"))) {
        if (!memory_ || !memory_->isAvailable() || !data.value(QStringLiteral("memory")).isArray() ||
            memories.size() > 10000)
            return failed(BackupError::ImportValidationFailure, QStringLiteral("Memory unavailable or invalid"));
        if (memory_->hasManagedMemory()) {
            return failed(
                BackupError::ImportConflict,
                QStringLiteral("Legacy memory import cannot replace or roll back managed records"));
        }
        QSet<QString> keys;
        for (const auto& item : memories) {
            const auto entry = item.toObject();
            const auto key = entry.value(QStringLiteral("key"));
            const auto value = entry.value(QStringLiteral("value"));
            if (!key.isString() || key.toString().size() > 1024 || keys.contains(key.toString()) ||
                !value.isString() || value.toString().size() > 65536)
                return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid memory entry"));
            keys.insert(key.toString());
            if (mode == ImportMode::Merge) {
                const auto existing = memory_->get(key.toString());
                if (!existing.isEmpty() && existing != value.toString())
                    return failed(BackupError::ImportConflict, QStringLiteral("Memory key conflict"));
            }
        }
    }
    const auto chats = data.value(QStringLiteral("chat")).toArray();
    if (selectedDomains.contains(QStringLiteral("chat"))) {
        if (!conversations_ || conversations_->status() != ConversationStoreStatus::Ready ||
            !data.value(QStringLiteral("chat")).isArray() || chats.size() > 100)
            return failed(BackupError::ImportValidationFailure, QStringLiteral("Chat unavailable or invalid"));
        if (mode == ImportMode::ReplaceSelectedDomains)
            return failed(BackupError::ImportConflict, QStringLiteral("Chat replacement is unsupported"));
        for (const auto& item : chats) {
            const auto chat = item.toObject();
            if (!chat.value(QStringLiteral("title")).isString() ||
                chat.value(QStringLiteral("title")).toString().size() > 200 ||
                !chat.value(QStringLiteral("messages")).isArray() ||
                chat.value(QStringLiteral("messages")).toArray().size() > 1000)
                return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid chat"));
            for (const auto& message : chat.value(QStringLiteral("messages")).toArray()) {
                const auto object = message.toObject();
                const auto role = object.value(QStringLiteral("role")).toString();
                if (!QStringList{QStringLiteral("user"), QStringLiteral("assistant"),
                                 QStringLiteral("system")}.contains(role) ||
                    !object.value(QStringLiteral("content")).isString() ||
                    object.value(QStringLiteral("content")).toString().size() > 65536 ||
                    !QDateTime::fromString(object.value(QStringLiteral("timestamp")).toString(),
                                           Qt::ISODateWithMs).isValid())
                    return failed(BackupError::ImportValidationFailure, QStringLiteral("Invalid chat message"));
            }
        }
    }

    // Capture bounded domain-owned state before the first commit. Chat merge creates only
    // new conversations, so rollback removes those IDs without touching existing chats.
    const QString oldTheme = settings_.themeName();
    const QString oldLanguage = settings_.appLanguage();
    const QString oldNetwork = settings_.networkMode();
    const QString oldProfiles = settings_.workspaceProfilesJson();
    const QString oldExtensions = settings_.mcpServersJson();
    const MemoryEntries oldMemory = selectedDomains.contains(QStringLiteral("memory"))
        ? memory_->entries() : MemoryEntries{};
    QStringList createdChats;
    auto rollback = [&](const QString& cause) -> BackupResult {
        QStringList failures;
        for (auto it = createdChats.crbegin(); it != createdChats.crend(); ++it)
            if (!conversations_->discardImportedConversation(*it)) failures.append(QStringLiteral("chat"));
        if (selectedDomains.contains(QStringLiteral("memory"))) {
            memory_->clear();
            if (!memory_->lastError().isEmpty()) failures.append(QStringLiteral("memory clear"));
            for (const auto& [key, value] : oldMemory) {
                memory_->put(key, value);
                if (!memory_->lastError().isEmpty()) { failures.append(QStringLiteral("memory restore")); break; }
            }
        }
        if (selectedDomains.contains(QStringLiteral("extensions")))
            settings_.setMcpServersJson(oldExtensions);
        if (selectedDomains.contains(QStringLiteral("workspaceProfiles")))
            settings_.setWorkspaceProfilesJson(oldProfiles);
        if (selectedDomains.contains(QStringLiteral("settings"))) {
            settings_.setNetworkMode(oldNetwork);
            settings_.setAppLanguage(oldLanguage);
            settings_.setThemeName(oldTheme);
        }
        if (!settings_.storageErrorCode().isEmpty() ||
            settings_.mcpServersJson() != oldExtensions ||
            settings_.workspaceProfilesJson() != oldProfiles ||
            settings_.networkMode() != oldNetwork ||
            settings_.appLanguage() != oldLanguage || settings_.themeName() != oldTheme)
            failures.append(QStringLiteral("settings restore"));
        if (!failures.isEmpty()) {
            bool recorded = true;
            for (const auto& domain : selectedDomains)
                recorded = RecoveryService::recordCondition(QStringLiteral("import-rollback"), domain,
                    QStringLiteral("ImportRollbackFailure"), QStringLiteral("inspect-import-recovery")) && recorded;
            return failed(BackupError::ImportRollbackFailure,
                          cause + QStringLiteral("; recovery required: ") + failures.join(QLatin1String(", ")) +
                          (recorded ? QString{} : QStringLiteral("; recovery status could not be persisted")));
        }
        return failed(BackupError::ImportCommitFailure, cause);
    };

    if (selectedDomains.contains(QStringLiteral("settings")) &&
        mode == ImportMode::ReplaceSelectedDomains) {
        settings_.setThemeName(settings.value(QStringLiteral("theme")).toString());
        settings_.setAppLanguage(settings.value(QStringLiteral("language")).toString());
        settings_.setNetworkMode(settings.value(QStringLiteral("networkMode")).toString());
    }
    if (selectedDomains.contains(QStringLiteral("workspaceProfiles")) &&
        (mode == ImportMode::ReplaceSelectedDomains || settings_.workspaceProfilesJson().isEmpty()))
        settings_.setWorkspaceProfilesJson(profiles.toString());
    if (selectedDomains.contains(QStringLiteral("extensions")))
        settings_.setMcpServersJson(QString::fromUtf8(QJsonDocument(QJsonObject{
            {QStringLiteral("mcpServers"), extensions}}).toJson(QJsonDocument::Compact)));
    if (!settings_.storageErrorCode().isEmpty() ||
        (selectedDomains.contains(QStringLiteral("settings")) &&
         mode == ImportMode::ReplaceSelectedDomains &&
         (settings_.themeName() != settings.value(QStringLiteral("theme")).toString() ||
          settings_.appLanguage() != settings.value(QStringLiteral("language")).toString() ||
          settings_.networkMode() != settings.value(QStringLiteral("networkMode")).toString())) ||
        (selectedDomains.contains(QStringLiteral("workspaceProfiles")) &&
         (mode == ImportMode::ReplaceSelectedDomains || oldProfiles.isEmpty()) &&
         settings_.workspaceProfilesJson() != profiles.toString()))
        return rollback(QStringLiteral("Settings import failed"));
    if (selectedDomains.contains(QStringLiteral("memory"))) {
        if (mode == ImportMode::ReplaceSelectedDomains) {
            memory_->clear();
            if (!memory_->lastError().isEmpty())
                return rollback(QStringLiteral("Memory clear failed"));
        }
        for (const auto& item : memories) {
            const auto entry = item.toObject();
            memory_->put(entry.value(QStringLiteral("key")).toString(),
                         entry.value(QStringLiteral("value")).toString());
            if (!memory_->lastError().isEmpty())
                return rollback(QStringLiteral("Memory import failed"));
        }
    }
    if (selectedDomains.contains(QStringLiteral("chat"))) {
        for (const auto& item : chats) {
            const auto chat = item.toObject();
            const auto created = conversations_->createConversation(
                chat.value(QStringLiteral("title")).toString());
            if (created.id.isEmpty())
                return rollback(QStringLiteral("Chat creation failed"));
            createdChats.append(created.id);
            int id = 1;
            for (const auto& itemMessage : chat.value(QStringLiteral("messages")).toArray()) {
                const auto message = itemMessage.toObject();
                const auto role = message.value(QStringLiteral("role")).toString();
                ConversationMessageRecord record;
                record.conversationId = created.id;
                record.messageId = id++;
                record.role = role == QLatin1String("user") ? ChatRole::User
                            : role == QLatin1String("assistant") ? ChatRole::Assistant : ChatRole::System;
                record.content = message.value(QStringLiteral("content")).toString();
                record.timestampUtc = QDateTime::fromString(
                    message.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
                record.status = ChatMessageStatus::Completed;
                if (!conversations_->appendMessage(record))
                    return rollback(QStringLiteral("Chat message import failed"));
            }
        }
    }
    bool clearedRecovery = true;
    for (const auto& domain : selectedDomains)
        clearedRecovery = RecoveryService::clearCondition(
            QStringLiteral("import-rollback"), domain) && clearedRecovery;
    return {BackupError::None, {}, clearedRecovery ? QString{} :
        QStringLiteral("Import committed, but recovery status could not be cleared")};
}
BackupResult BackupService::exportScopedMemory(IMemoryStore& store, MemoryScope scope,
                                               const MemoryManagementContext& context) {
    QList<ScopedMemoryRecord> records;
    QString cursor;
    do {
        const auto page = store.listManagedMemory({scope, {}, cursor, 50}, context);
        if (page.status != MemoryManagementStatus::Ready) {
            return {BackupError::ExportFailure, {}, memoryManagementStatusName(page.status)};
        }
        records.append(page.records);
        cursor = page.nextCursor;
        if (records.size() > 500) {
            return {BackupError::ExportFailure, {}, "LimitExceeded"};
        }
    } while (!cursor.isEmpty());
    for (const auto& r : records) {
        if (r.corrections.size() >= 64) {
            return {BackupError::ExportFailure, {}, "LimitExceeded"};
        }
    }
    const auto bytes = encodeMemoryArchive(records);
    if (decodeMemoryArchive(bytes).status != MemoryManagementStatus::Ready) {
        return {BackupError::ExportFailure, {}, "InvalidInput"};
    }
    if (bytes.size() > 98304) {
        return {BackupError::ExportFailure, {}, "LimitExceeded"};
    }
    return {BackupError::None, bytes, {}};
}
BackupResult BackupService::importScopedMemory(IMemoryStore& store, MemoryScope scope,
                                               const MemoryManagementContext& context,
                                               const QByteArray& data, bool skipIdentical) {
    if (!authorizedMemoryScope(scope, context)) {
        return {BackupError::ImportValidationFailure, {}, "Unauthorized"};
    }
    const auto parsed = decodeMemoryArchive(data);
    if (parsed.status != MemoryManagementStatus::Ready) {
        return {
            BackupError::ImportValidationFailure, {}, memoryManagementStatusName(parsed.status)};
    }
    QList<MemoryMutation> changes;
    for (const auto& r : parsed.records) {
        if (r.scope != scope || !manageableMemory(r, context)) {
            return {BackupError::ImportValidationFailure, {}, "Unauthorized"};
        }
        MemoryMutation m;
        m.kind = MemoryMutation::Kind::Import;
        m.record = r;
        m.skipIdentical = skipIdentical;
        changes.append(m);
    }
    if (changes.isEmpty()) {
        return {};
    }
    const auto applied = store.mutateManagedMemory(changes, context);
    if (applied.status != MemoryManagementStatus::Ready) {
        return {BackupError::ImportCommitFailure, {}, memoryManagementStatusName(applied.status)};
    }
    return {};
}

} // namespace sentinel::core
