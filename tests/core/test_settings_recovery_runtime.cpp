// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/SettingsService.h"
#include "sentinel/core/app/OnboardingService.h"
#include "sentinel/core/app/RecoveryService.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/network/NetworkPolicyService.h"
#include "sentinel/core/privacy/RetentionPolicy.h"
#include "sentinel/core/runtime/LocalInference.h"
#include "sentinel/core/agent/SQLiteAgentRunStore.h"
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QTranslator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest>

using namespace sentinel::core;

class SettingsRecoveryRuntimeTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        QStandardPaths::setTestModeEnabled(true);
        QCoreApplication::setApplicationName(QStringLiteral("sentinel-settings-cert-") +
            QUuid::createUuid().toString(QUuid::WithoutBraces));
    }
    void cleanupTestCase() {
        QFile::remove(RecoveryService::statusPath());
        QDir().rmdir(QFileInfo(RecoveryService::statusPath()).absolutePath());
        NetworkPolicyService::instance().setMode(NetworkMode::Online);
    }
    void themeCatalogMatchesValidatedChoices() {
        QTemporaryDir dir;
        AppSettings settings(std::make_unique<JsonSettingsStore>(dir.filePath("settings.json")), inMemoryTestCredentialStore());
        SettingsService service(settings);
        QStringList advertised;
        for (const auto& row : service.snapshots())
            if (row.id == QStringLiteral("appearance.theme")) advertised = row.allowedValues;
        QCOMPARE(advertised, AppSettings::availableThemes());
        QCOMPARE(advertised.size(), 14);
        for (const auto& theme : advertised) {
            QVERIFY(service.set(QStringLiteral("appearance.theme"), theme).accepted);
            QCOMPARE(settings.themeName(), theme);
        }
        QVERIFY(!service.set(QStringLiteral("appearance.theme"), QStringLiteral("Unknown theme")).accepted);
    }
    void settingsDiskRoundTripAndReset() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("settings.json");
        {
            AppSettings settings(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
            ModelService models(&settings);
            SettingsService surface(settings, &models);
            QVERIFY(surface.set("general.language", "tr").accepted);
            QVERIFY(surface.set("network.mode", "Offline").accepted);
            QVERIFY(surface.set("appearance.theme", "Dracula").accepted);
            QVERIFY(surface.set("models.provider", "llama-cpp-server").accepted);
            settings.setSelectedModelForProvider("llama-cpp-server", "existing-model");
            settings.setSelectedWorkspaceId("personal");
            QVERIFY(!surface.set("general.language", "xx").accepted);
            QVERIFY(!surface.set("network.mode", "Bogus").accepted);
            QCOMPARE(settings.appLanguage(), QStringLiteral("tr"));
            QCOMPARE(settings.networkMode(), QStringLiteral("Offline"));
        }
        AppSettings restored(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
        QCOMPARE(restored.appLanguage(), QStringLiteral("tr"));
        QCOMPARE(restored.networkMode(), QStringLiteral("Offline"));
        QCOMPARE(restored.themeName(), QStringLiteral("Dracula"));
        QCOMPARE(restored.selectedRuntimeProvider(), QStringLiteral("llama-cpp-server"));
        QCOMPARE(restored.selectedModelForProvider("llama-cpp-server"), QStringLiteral("existing-model"));
        QCOMPARE(restored.selectedWorkspaceId(), QStringLiteral("personal"));
        SettingsService surface(restored);
        QVERIFY(surface.reset("general.language").accepted);
        QCOMPARE(restored.appLanguage(), QStringLiteral("en"));
        QCOMPARE(restored.networkMode(), QStringLiteral("Offline"));
        QCOMPARE(restored.themeName(), QStringLiteral("Dracula"));
    }
    void corruptSettingsAndWriteFailurePreserveDisk() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const auto path = dir.filePath("settings.json");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray corrupt("{broken");
        QCOMPARE(file.write(corrupt), qint64(corrupt.size()));
        file.close();
        AppSettings settings(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
        QCOMPARE(settings.storageErrorCode(), QStringLiteral("CorruptState"));
        QVERIFY(!SettingsService(settings).set("general.language", "tr").accepted);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), corrupt);
        file.close();
        // Existing regular file as parent: deterministic failure even with root privileges.
        JsonSettingsStore impossible(path + "/child.json");
        impossible.setValue("unrelated", "value");
        QCOMPARE(impossible.errorCode(), QStringLiteral("StoreUnavailable"));
        QVERIFY(impossible.value("unrelated").isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), corrupt);
    }
    void productSurfacesReportWriteFailure() {
        QTemporaryDir dir;
        const auto parent = dir.filePath("blocked");
        QFile blocker(parent);
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.write("fixture");
        blocker.close();
        AppSettings settings(std::make_unique<JsonSettingsStore>(parent + "/settings.json"),
                             inMemoryTestCredentialStore());
        const auto result = SettingsService(settings).set("general.language", "tr");
        QVERIFY(!result.accepted);
        QCOMPARE(result.code, QStringLiteral("StoreUnavailable"));
        QVERIFY(!OnboardingService(settings).chooseProcessingMode("cloud").accepted);
        QVERIFY(!OnboardingService(settings).finish().accepted);
        QVERIFY(!settings.onboardingComplete());
    }
    void onboardingFirstRunResumeAndFinishWithoutProvider() {
        QTemporaryDir dir;
        const auto path = dir.filePath("settings.json");
        {
            AppSettings settings(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
            OnboardingService onboarding(settings);
            QVERIFY(!onboarding.snapshot().complete);
            QVERIFY(!onboarding.snapshot().resumed);
            QCOMPARE(onboarding.snapshot().step, OnboardingStep::Welcome);
            QVERIFY(onboarding.chooseProcessingMode("local").accepted);
            QVERIFY(!onboarding.chooseProcessingMode("invalid").accepted);
            QVERIFY(!onboarding.chooseModel("missing", "missing").accepted);
            QVERIFY(onboarding.advance().accepted);
        }
        {
            AppSettings settings(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
            OnboardingService onboarding(settings);
            QVERIFY(onboarding.snapshot().resumed);
            QCOMPARE(onboarding.snapshot().step, OnboardingStep::ProcessingMode);
            for (int i = 0; i < 5; ++i) QVERIFY(onboarding.advance(true).accepted);
            QVERIFY(onboarding.finish().accepted);
            QVERIFY(settings.onboardingComplete());
        }
        AppSettings restored(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
        QVERIFY(OnboardingService(restored).snapshot().complete);
        QVERIFY(!OnboardingService(restored).advance().accepted);
        QCOMPARE(WorkspaceService{}.resolveProfile(restored.workspaceProfilesJson(), "personal")
            .configured.value("privacy").toString(), QStringLiteral("local-only"));
    }
    void credentialsStayOutOfSettingsBackupAndSummaries() {
        QTemporaryDir dir;
        const auto path = dir.filePath("settings.json");
        auto credentials = inMemoryTestCredentialStore();
        AppSettings settings(std::make_unique<JsonSettingsStore>(path), credentials);
        SettingsService surface(settings);
        const QString safeFixture = QStringLiteral("not-a-real-provider-secret-certification");
        QVERIFY(surface.setProviderCredential("gemini", safeFixture).accepted);
        QCOMPARE(settings.credentialState("gemini"), QStringLiteral("Configured"));
        QVERIFY(!settings.geminiApiKey().isEmpty());
        settings.setAppLanguage("tr");
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(!file.readAll().contains(safeFixture.toUtf8()));
        auto backup = surface.exportBackupJson({"settings"});
        QVERIFY(backup.succeeded());
        QVERIFY(!backup.data.contains(safeFixture.toUtf8()));
        for (const auto& row : surface.snapshots())
            QVERIFY(!row.value.toString().contains(safeFixture));
        QVERIFY(surface.clearCredential("gemini").accepted);
        QCOMPARE(settings.credentialState("gemini"), QStringLiteral("NotConfigured"));
        QVERIFY(settings.geminiApiKey().isEmpty());
        AppSettings unavailable(std::make_unique<JsonSettingsStore>(dir.filePath("disabled.json")),
            CredentialStore(std::make_shared<DisabledCredentialBackend>()));
        QVERIFY(!SettingsService(unavailable).setProviderCredential("gemini", safeFixture).accepted);
        QCOMPARE(unavailable.credentialState("gemini"), QStringLiteral("SecureStoreUnavailable"));
        QVERIFY(unavailable.geminiApiKey().isEmpty());
    }
    void legacyCredentialMigrationPreservesOriginalOnBackendFailure() {
        QTemporaryDir dir;
        const QString fixture = QStringLiteral("legacy-test-only-not-a-real-key");
        const auto path = dir.filePath("legacy.json");
        { JsonSettingsStore store(path); store.setValue("geminiApiKey", fixture); }
        AppSettings migrated(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
        QVERIFY(migrated.geminiApiKey() == fixture);
        QVERIFY(JsonSettingsStore(path).value("geminiApiKey").isEmpty());
        QCOMPARE(migrated.credentialState("gemini"), QStringLiteral("Configured"));
        const auto failedPath = dir.filePath("migration-failed.json");
        { JsonSettingsStore store(failedPath); store.setValue("geminiApiKey", fixture); }
        AppSettings failed(std::make_unique<JsonSettingsStore>(failedPath),
            CredentialStore(std::make_shared<DisabledCredentialBackend>()));
        // Audit existing loss-avoidance contract; this is not secure-persistence PASS.
        QCOMPARE(failed.credentialState("gemini"), QStringLiteral("MigrationRequired"));
        QVERIFY(failed.geminiApiKey() == fixture);
        QVERIFY(JsonSettingsStore(failedPath).value("geminiApiKey") == fixture);
    }
    void networkModesUseExactLoopbackContract() {
        QTemporaryDir dir;
        AppSettings settings(std::make_unique<JsonSettingsStore>(dir.filePath("settings.json")),
                             inMemoryTestCredentialStore());
        auto& policy = NetworkPolicyService::instance();
        for (const QString mode : {QStringLiteral("Online"), QStringLiteral("LocalOnly"),
                                    QStringLiteral("Offline")}) {
            QVERIFY(SettingsService(settings).set("network.mode", mode).accepted);
            QCOMPARE(policy.check(QUrl("http://127.0.0.1:8081/v1/chat/completions")), NetworkDecision::Allowed);
            QCOMPARE(policy.check(QUrl("http://localhost:1234/v1/models")), NetworkDecision::Allowed);
            QCOMPARE(policy.check(QUrl("https://generativelanguage.googleapis.com")),
                mode == "Online" ? NetworkDecision::Allowed :
                mode == "Offline" ? NetworkDecision::Offline : NetworkDecision::LocalOnly);
        }
    }
    void compiledLocalizationUsesEnglishAndTurkish() {
        for (const auto& language : {QStringLiteral("en"), QStringLiteral("tr")}) {
            QTranslator translator;
            QVERIFY(translator.load(QStringLiteral(CERTIFICATION_TRANSLATIONS) +
                "/sentinel_" + language + ".qm"));
            const auto settings = translator.translate("AppearanceSettingsTab", "Active Theme");
            const auto empty = translator.translate("HomeChatSurface", "Choose model");
            QVERIFY(!settings.isEmpty());
            QVERIFY(!empty.isEmpty());
            const auto welcome = translator.translate("WelcomeStep", "Local-first");
            const auto missing = translator.translate("ApplicationController", "Select a provider and model before sending.");
            QVERIFY(!welcome.isEmpty());
            QVERIFY(!missing.isEmpty());
            if (language == "tr") {
                QCOMPARE(settings, QStringLiteral("Etkin Tema"));
                QCOMPARE(empty, QStringLiteral("Model seçin"));
                QCOMPARE(welcome, QStringLiteral("Önce yerel"));
                QCOMPARE(missing, QStringLiteral("Göndermeden önce bir sağlayıcı ve model seçin."));
            } else {
                QCOMPARE(settings, QStringLiteral("Active Theme"));
                QCOMPARE(empty, QStringLiteral("Choose model"));
            }
        }
    }
    void actualLocalTransportAndBlockedCloudDoNotFallback() {
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost, 0));
        int requests = 0;
        connect(&server, &QTcpServer::newConnection, &server, [&] {
            auto* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            auto pending = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, socket, [&, socket, pending] {
                pending->append(socket->readAll());
                if (!pending->contains("\r\n\r\n") || socket->property("responded").toBool()) return;
                socket->setProperty("responded", true);
                ++requests;
                const QByteArray body("{\"choices\":[{\"message\":{\"content\":\"TRANSPORT_OK\"},\"finish_reason\":\"stop\"}]}");
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                    QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        });
        LMStudioConfig local;
        local.endpoint = QUrl(QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort()));
        auto& policy = NetworkPolicyService::instance();
        for (auto mode : {NetworkMode::Online, NetworkMode::LocalOnly, NetworkMode::Offline}) {
            policy.setMode(mode);
            const auto result = LMStudioLocalInferenceClient(local, 1000).completeOpenAiChat({});
            QVERIFY(result.ok);
            QCOMPARE(result.body.value("choices").toArray().first().toObject()
                .value("message").toObject().value("content").toString(), QStringLiteral("TRANSPORT_OK"));
        }
        QCOMPARE(requests, 3);
        LMStudioConfig cloud;
        cloud.endpoint = QUrl("https://api.openai.com");
        // Fixture never leaves this process: both tested modes reject before transport.
        cloud.apiKey = QStringLiteral("test-only-never-transmitted");
        for (auto mode : {NetworkMode::LocalOnly, NetworkMode::Offline}) {
            policy.setMode(mode);
            const auto result = LMStudioLocalInferenceClient(cloud, 1000).completeOpenAiChat({});
            QVERIFY(!result.ok);
            QCOMPARE(result.providerErrorCategory, int(ChatProviderErrorCategory::Offline));
            QCOMPARE(result.httpStatus, 0);
            QCOMPARE(requests, 3);
        }
        policy.setMode(NetworkMode::Online);
        cloud.apiKey.clear();
        QVERIFY(!LMStudioLocalInferenceClient(cloud, 1000).completeOpenAiChat({}).ok);
        QCOMPARE(requests, 3);
    }
    void invalidPersistedChoicesDoNotResetUnrelatedSettings() {
        QTemporaryDir dir;
        const auto path = dir.filePath("settings.json");
        {
            JsonSettingsStore store(path);
            store.setValue("appLanguage", "unsupported");
            store.setValue("networkMode", "unsupported");
            store.setValue("themeName", "Dracula");
            store.setValue("unrelated-certification", "KEEP");
        }
        AppSettings settings(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
        QVERIFY(settings.availableLanguages().contains(settings.appLanguage()));
        QCOMPARE(settings.networkMode(), QStringLiteral("Online"));
        QCOMPARE(settings.themeName(), QStringLiteral("Dracula"));
        QCOMPARE(JsonSettingsStore(path).value("unrelated-certification"), QStringLiteral("KEEP"));
    }
    void agentHistoryRetentionProtectsActiveAndRecentRuns() {
        QTemporaryDir dir;
        SQLiteAgentRunStore store(dir.filePath("runs.sqlite3"));
        auto run = [&](const QString& id, int days, bool complete) {
            AgentEvent event;
            event.id = id + "-start";
            event.sessionId = id;
            event.turnId = id;
            event.timestamp = QDateTime::currentDateTimeUtc().addDays(-days);
            event.type = AgentEventType::RunStarted;
            event.payload = AgentRunStartedEvent{};
            if (!store.record(event)) return false;
            if (!complete) return true;
            event.id = id + "-end";
            event.type = AgentEventType::AgentCompleted;
            event.payload = AgentTextEvent{QStringLiteral("DONE")};
            return store.record(event);
        };
        QVERIFY(run("old", 3, true));
        QVERIFY(run("recent", 0, true));
        QVERIFY(run("active", 3, false));
        QCOMPARE(store.pruneCompletedBefore(QDateTime::currentDateTimeUtc().addDays(-1)), 1);
        QVERIFY(store.runById("old").runId.isEmpty());
        QVERIFY(!store.runById("recent").runId.isEmpty());
        QCOMPARE(store.runById("active").state, QStringLiteral("Running"));
    }
    void backupDiskRestoreAndCorruptRejection() {
        QTemporaryDir dir;
        const auto path = dir.filePath("settings.json");
        AppSettings settings(std::make_unique<JsonSettingsStore>(path), inMemoryTestCredentialStore());
        SQLiteConversationStore chat(dir.filePath("chat.sqlite3"));
        SQLiteMemoryStore memory(dir.filePath("memory.sqlite3"));
        QVERIFY(memory.isAvailable());
        QCOMPARE(chat.status(), ConversationStoreStatus::Ready);
        settings.setAppLanguage("tr");
        settings.setThemeName("Dracula");
        settings.setWorkspaceProfilesJson("{\"version\":1,\"workspaces\":{},\"presets\":{}}");
        memory.put("certification", "MEMORY_OK");
        auto conversation = chat.createConversation("CERTIFICATION_CHAT");
        ConversationMessageRecord message;
        message.conversationId = conversation.id;
        message.messageId = 1;
        message.role = ChatRole::User;
        message.content = "CHAT_OK";
        message.timestampUtc = QDateTime::currentDateTimeUtc();
        message.status = ChatMessageStatus::Completed;
        QVERIFY(chat.appendMessage(message));
        BackupService backup(settings, &chat, &memory);
        QVERIFY(
            !backup.exportJson({"settings", "workspaceProfiles", "chat", "memory"}).succeeded());
        auto exported = backup.exportJson({"chat"});
        const auto settingsExport = backup.exportJson({"settings"});
        QVERIFY(settingsExport.succeeded());
        QVERIFY(exported.succeeded());
        QFile disk(dir.filePath("backup.json"));
        QVERIFY(disk.open(QIODevice::WriteOnly));
        QCOMPARE(disk.write(exported.data), qint64(exported.data.size()));
        disk.close();
        QVERIFY(disk.open(QIODevice::ReadOnly));
        const auto bytes = disk.readAll();
        QVERIFY(QJsonDocument::fromJson(bytes).object().value("manifest").toObject()
            .value("credentialsExcluded").toBool());
        settings.setAppLanguage("en");
        settings.setThemeName("Liquid Glass Light");
        memory.put("certification", "MODIFIED");
        QVERIFY(
            backup.importJson(settingsExport.data, {"settings"}, ImportMode::ReplaceSelectedDomains)
                .succeeded());
        QCOMPARE(settings.appLanguage(), QStringLiteral("tr"));
        QCOMPARE(memory.get("certification"), QStringLiteral("MODIFIED"));
        QVERIFY(backup.importJson(bytes, {"chat"}, ImportMode::Merge).succeeded());
        QCOMPARE(chat.listConversations().size(), 2);
        QCOMPARE(chat.loadMessages(chat.listConversations().last().id).first().content, QStringLiteral("CHAT_OK"));
        auto rejected = backup.importJson("{broken", {"settings", "memory"}, ImportMode::ReplaceSelectedDomains);
        QCOMPARE(rejected.error, BackupError::ImportValidationFailure);
        QCOMPARE(settings.appLanguage(), QStringLiteral("tr"));
        QCOMPARE(memory.get("certification"), QStringLiteral("MODIFIED"));
        QCOMPARE(chat.listConversations().size(), 2);
        QVERIFY(!backup.exportJson({"credentials"}).succeeded());
    }
    void coordinatedOperationsRefuseBeforeMutation() {
        QTemporaryDir dir;
        const auto settingsPath = dir.filePath("settings.json");
        AppSettings settings(std::make_unique<JsonSettingsStore>(settingsPath),
                             inMemoryTestCredentialStore());
        SQLiteConversationStore chat(dir.filePath("conversations.sqlite3"));
        SQLiteMemoryStore memory(dir.filePath("memory.sqlite3"));
        BackupService backup(settings, &chat, &memory);
        settings.setThemeName("Dracula");
        const auto conversation = chat.createConversation("Preserve identity");
        QVERIFY(!conversation.id.isEmpty());
        const auto original = backup.exportJson({"settings"});
        QVERIFY(original.succeeded());
        auto root = QJsonDocument::fromJson(original.data).object();
        auto manifest = root.value("manifest").toObject();
        manifest.insert("includedDomains", QJsonArray{"settings", "chat"});
        root.insert("manifest", manifest);
        auto data = root.value("data").toObject();
        data.insert("chat", QJsonArray{});
        root.insert("data", data);
        settings.setThemeName("Liquid Glass Light");
        QFile file(settingsPath);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto preimage = file.readAll();
        file.close();
        for (const auto mode : {ImportMode::Merge, ImportMode::ReplaceSelectedDomains}) {
            const auto result =
                backup.importJson(QJsonDocument(root).toJson(), {"settings", "chat"}, mode);
            QCOMPARE(result.error, BackupError::CoordinatedOperationBlocked);
            QVERIFY(!result.succeeded());
        }
        const auto exported = backup.exportJson({"settings", "chat"});
        QCOMPARE(exported.error, BackupError::CoordinatedOperationBlocked);
        QVERIFY(exported.data.isEmpty());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), preimage);
        QCOMPARE(settings.themeName(), QString("Liquid Glass Light"));
        QCOMPARE(chat.listConversations().size(), 1);
        QCOMPARE(chat.listConversations().first().id, conversation.id);
        const auto state = SettingsService(settings).backupAvailability();
        QCOMPARE(state.value("coordinatedState").toString(), QString("Blocked"));
        QVERIFY(!state.value("coordinatedBackupAvailable").toBool());
        QVERIFY(!state.value("coordinatedRestoreAvailable").toBool());
        QVERIFY(state.value("includedDomains").toArray().isEmpty());
        QVERIFY(state.value("lastVerifiedCoordinatedBackup").isNull());
        QVERIFY(!state.value("chat").toBool());
        QVERIFY(!state.value("memory").toBool());
        QCOMPARE(state.value("maximumSelectedDomains").toInt(), 1);
        QVERIFY(!QJsonDocument(state).toJson().contains(dir.path().toUtf8()));
    }
    void retentionAndClearAreStoreScoped() {
        QTemporaryDir dir;
        AppSettings settings(std::make_unique<JsonSettingsStore>(dir.filePath("settings.json")), inMemoryTestCredentialStore());
        SQLiteConversationStore chat(dir.filePath("chat.sqlite3"));
        SQLiteMemoryStore memory(dir.filePath("memory.sqlite3"));
        auto add = [&](const QString& title, int days, ChatMessageStatus status) {
            const auto conversation = chat.createConversation(title);
            ConversationMessageRecord message;
            message.conversationId = conversation.id;
            message.messageId = 1;
            message.content = title;
            message.timestampUtc = QDateTime::currentDateTimeUtc().addDays(-days);
            message.status = status;
            if (!chat.appendMessage(message)) return false;
            // Retention owns conversation update time, not the message timestamp.
            const auto connection = QStringLiteral("retention-fixture-") + conversation.id;
            bool updated = false;
            {
                auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
                database.setDatabaseName(chat.databasePath());
                if (database.open()) {
                    QSqlQuery query(database);
                    query.prepare("UPDATE conversations SET updated_at=? WHERE id=?");
                    query.addBindValue(message.timestampUtc.toUTC().toString(Qt::ISODateWithMs));
                    query.addBindValue(conversation.id);
                    updated = query.exec();
                }
                database.close();
            }
            QSqlDatabase::removeDatabase(connection);
            return updated;
        };
        QVERIFY(add("old completed", 3, ChatMessageStatus::Completed));
        QVERIFY(add("recent", 0, ChatMessageStatus::Completed));
        QVERIFY(add("old active", 3, ChatMessageStatus::Streaming));
        memory.put("keep", "MEMORY_RETAINED");
        QVERIFY(RetentionPolicy::set(settings, "chat", "1d"));
        const auto result = RetentionPolicy::maintain(settings, &chat, nullptr, nullptr);
        QVERIFY(!result.value("partialFailure").toBool());
        QCOMPARE(result.value("removed").toInt(), 1);
        QCOMPARE(chat.listConversations().size(), 2);
        QCOMPARE(memory.get("keep"), QStringLiteral("MEMORY_RETAINED"));
        SettingsService surface(settings, nullptr, nullptr, nullptr, nullptr, &chat, &memory);
        QVERIFY(surface.clearMemory().accepted);
        QCOMPARE(chat.listConversations().size(), 2);
        memory.put("keep", "MEMORY_RETAINED");
        QVERIFY(surface.clearChatHistory().accepted);
        QVERIFY(chat.listConversations().isEmpty());
        QCOMPARE(memory.get("keep"), QStringLiteral("MEMORY_RETAINED"));
        QCOMPARE(settings.networkMode(), QStringLiteral("Online"));
    }
    void interruptedDatabaseRecoveryDoesNotReplay() {
        QTemporaryDir dir;
        const auto path = dir.filePath("chat.sqlite3");
        QString id;
        {
            SQLiteConversationStore chat(path);
            QCOMPARE(chat.status(), ConversationStoreStatus::Ready);
            id = chat.createConversation("interrupted fixture").id;
            ConversationMessageRecord message;
            message.conversationId = id;
            message.messageId = 1;
            message.content = "PARTIAL";
            message.timestampUtc = QDateTime::currentDateTimeUtc();
            message.status = ChatMessageStatus::Streaming;
            QVERIFY(chat.appendMessage(message));
        }
        SQLiteConversationStore reopened(path);
        QCOMPARE(reopened.loadMessages(id).first().status, ChatMessageStatus::Interrupted);
        const auto state = RecoveryService(&reopened).state();
        QCOMPARE(state.value("health").toString(), QStringLiteral("Recoverable"));
        QCOMPARE(reopened.loadMessages(id).first().content, QStringLiteral("PARTIAL"));
        const auto malformed = dir.filePath("invalid.sqlite3");
        QFile disk(malformed);
        QVERIFY(disk.open(QIODevice::WriteOnly));
        disk.write("not a database");
        disk.close();
        SQLiteConversationStore invalid(malformed);
        QCOMPARE(invalid.status(), ConversationStoreStatus::Unavailable);
        QVERIFY(disk.open(QIODevice::ReadOnly));
        QCOMPARE(disk.readAll(), QByteArray("not a database"));
    }
};
QTEST_MAIN(SettingsRecoveryRuntimeTest)
#include "test_settings_recovery_runtime.moc"
