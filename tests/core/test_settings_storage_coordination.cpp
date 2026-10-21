// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/SettingsService.h"
#include "sentinel/core/app/WorkspaceService.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include "sentinel/core/model/ModelService.h"
#include "sentinel/core/platform/DpapiEncryptedSettingsStore.h"
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <future>
using namespace sentinel::core;
using namespace std::chrono_literals;
namespace {
QByteArray bytes(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
std::unique_ptr<ISettingsStore> encrypted(const QString& path) {
    return std::make_unique<DpapiEncryptedSettingsStore>(std::make_unique<JsonSettingsStore>(path),
                                                         [] { return QByteArray(32, 'k'); });
}
} // namespace
class SettingsStorageCoordinationTest final : public QObject {
    Q_OBJECT
private slots:
    void transientFallbackReaderNeverMigratesLegacyCredentials() {
        QTemporaryDir profile;
        const auto path = profile.filePath("settings.json");
        auto credentials = inMemoryTestCredentialStore();
        JsonSettingsStore seed(path);
        seed.setValue("openAiApiKey", "fixture-legacy-value");
        const auto before = bytes(path);
        AppSettings settings(std::make_unique<JsonSettingsStore>(path), credentials);
        settings.disablePersistence();
        QCOMPARE(settings.openAiApiKey(), QString("fixture-legacy-value"));
        QCOMPARE(settings.storageErrorCode(), QString("SettingsReadOnly"));
        QVERIFY(!credentials.readCredential({"openai", "apiKey"}).secret.has_value());
        QCOMPARE(bytes(path), before);
    }
    void unsupportedAtomicOwnerRefusesWithoutPartialFallback() {
        class SingleKeyOwner final : public ISettingsStore {
        public:
            QString value(const QString&, const QString& fallback = {}) const override {
                return fallback;
            }
            void setValue(QString, QString) override {
                ++writes;
            }
            void remove(const QString&) override {
                ++writes;
            }
            int writes = 0;
        };
        auto owner = std::make_unique<SingleKeyOwner>();
        auto* observed = owner.get();
        AppSettings settings(std::move(owner), inMemoryTestCredentialStore());
        const auto created = WorkspaceService{}.createWorkspace({}, "Fixture", "Personal");
        QVERIFY(!settings.persistWorkspaceState(created.catalogJson, created.selectedWorkspaceId));
        QCOMPARE(settings.storageErrorCode(), QString("AtomicSettingsUnsupported"));
        QCOMPARE(observed->writes, 0);
    }
    void portableSettingsPublishTogetherOrRefuse() {
        QTemporaryDir profile;
        const auto path = profile.filePath("settings.json");
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(encrypted(path), inMemoryTestCredentialStore());
        settings.attachStorageAdmission(admission);
        QVERIFY(settings.applyPortableSettings("Paper", "en", "Online"));
        const auto before = bytes(path);
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        QVERIFY(!settings.applyPortableSettings("Linen", "tr", "Offline"));
        QCOMPARE(bytes(path), before);
        QCOMPARE(settings.themeName(), QString("Paper"));
        QCOMPARE(settings.networkMode(), QString("Online"));
    }
    void settingsRefusalPreservesDiskCacheAndNotifications() {
        QTemporaryDir profile;
        const auto path = profile.filePath("settings.json");
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(encrypted(path), inMemoryTestCredentialStore());
        QVERIFY(settings.attachStorageAdmission(admission));
        settings.setThemeName("Paper");
        QCOMPARE(settings.themeName(), QString("Paper"));
        const auto before = bytes(path);
        QSignalSpy changed(&settings, &AppSettings::themeNameChanged);
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        SettingsService service(settings);
        const auto rejected = service.set("appearance.theme", "Linen");
        QVERIFY(!rejected.accepted);
        QCOMPARE(rejected.code, QString("StorageBusy"));
        QCOMPARE(settings.themeName(), QString("Paper"));
        QCOMPARE(bytes(path), before);
        QCOMPARE(changed.count(), 0);
        QCOMPARE(admission->resume(), StorageAdmission::Result::Ready);
        QVERIFY(service.set("appearance.theme", "Linen").accepted);
        QCOMPARE(changed.count(), 1);
    }
    void workspacePublicationIsAtomicBeforeNotifications() {
        QTemporaryDir profile;
        const auto path = profile.filePath("settings.json");
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(encrypted(path), inMemoryTestCredentialStore());
        settings.attachStorageAdmission(admission);
        WorkspaceService workspaces;
        auto created = workspaces.createWorkspace({}, "Fixture", "Personal");
        QVERIFY(created.success);
        bool consistent = false;
        connect(&settings, &AppSettings::selectedWorkspaceIdChanged, &settings, [&] {
            JsonSettingsStore persisted(path);
            consistent = persisted.value("selectedWorkspaceId") == created.selectedWorkspaceId &&
                         persisted.value("workspaceCatalogJson") == created.catalogJson;
        });
        QVERIFY(settings.persistWorkspaceState(created.catalogJson, created.selectedWorkspaceId));
        QVERIFY(consistent);
        settings.setDefaultWorkspaceId(created.selectedWorkspaceId);
        auto renamed = workspaces.renameWorkspace(settings.workspaceCatalogJson(),
                                                  created.selectedWorkspaceId, "Edited");
        QVERIFY(renamed.success);
        QVERIFY(settings.persistWorkspaceState(renamed.catalogJson, created.selectedWorkspaceId));
        auto archived =
            workspaces.archiveWorkspace(renamed.catalogJson, created.selectedWorkspaceId);
        QVERIFY(archived.success);
        const auto next =
            workspaces.normalizedWorkspaceId(created.selectedWorkspaceId, archived.catalogJson);
        QVERIFY(settings.persistWorkspaceState(archived.catalogJson, next));
        QCOMPARE(settings.selectedWorkspaceId(), QString("personal"));
        QCOMPARE(settings.defaultWorkspaceId(), QString("personal"));
        auto removed =
            workspaces.deleteWorkspace(archived.catalogJson, created.selectedWorkspaceId, next);
        QVERIFY(removed.success);
        QVERIFY(settings.persistWorkspaceState(removed.catalogJson, removed.selectedWorkspaceId));
        JsonSettingsStore disk(path);
        QCOMPARE(disk.value("selectedWorkspaceId"), settings.selectedWorkspaceId());
        QCOMPARE(disk.value("workspaceCatalogJson"), settings.workspaceCatalogJson());
    }
    void workspaceRefusalIsAllOrNothing() {
        QTemporaryDir profile;
        const auto path = profile.filePath("settings.json");
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(encrypted(path), inMemoryTestCredentialStore());
        settings.attachStorageAdmission(admission);
        settings.setThemeName("Paper");
        const auto before = bytes(path);
        WorkspaceService workspaces;
        const auto created = workspaces.createWorkspace({}, "Fixture", "Coding");
        QSignalSpy changed(&settings, &AppSettings::workspaceSettingsChanged);
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        QVERIFY(!settings.persistWorkspaceState(created.catalogJson, created.selectedWorkspaceId));
        QCOMPARE(settings.storageErrorCode(), QString("StorageBusy"));
        QCOMPARE(bytes(path), before);
        QVERIFY(settings.workspaceCatalogJson().isEmpty());
        QCOMPARE(settings.selectedWorkspaceId(), QString("personal"));
        QCOMPARE(changed.count(), 0);
    }
    void failedPublicationRollsBackAuthoritativeCache() {
        QTemporaryDir profile;
        QVERIFY(QDir(profile.path()).mkdir("data"));
        const auto path = profile.filePath("data/settings.json");
        AppSettings settings(encrypted(path), inMemoryTestCredentialStore());
        settings.setThemeName("Paper");
        const auto before = bytes(path);
        QVERIFY(QDir(profile.path()).rename("data", "saved"));
        QFile blocker(profile.filePath("data"));
        QVERIFY(blocker.open(QIODevice::WriteOnly));
        blocker.close();
        QSignalSpy changed(&settings, &AppSettings::themeNameChanged);
        const auto created = WorkspaceService{}.createWorkspace({}, "Fixture", "Personal");
        QVERIFY(!settings.persistWorkspaceState(created.catalogJson, created.selectedWorkspaceId));
        QVERIFY(settings.workspaceCatalogJson().isEmpty());
        QCOMPARE(settings.selectedWorkspaceId(), QString("personal"));
        settings.setThemeName("Linen");
        QCOMPARE(settings.themeName(), QString("Paper"));
        QCOMPARE(changed.count(), 0);
        QVERIFY(!settings.storageErrorCode().isEmpty());
        QCOMPARE(bytes(profile.filePath("saved/settings.json")), before);
    }
    void lateCallbacksStayStaleAfterResumeAndReattachment() {
        QTemporaryDir profile;
        const auto path = profile.filePath("settings.json");
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(encrypted(path), inMemoryTestCredentialStore());
        QVERIFY(settings.attachStorageAdmission(admission));
        settings.setThemeName("Paper");
        const auto before = bytes(path);
        auto late = [&] { settings.setThemeName("Linen"); };
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        QCOMPARE(admission->enterMaintenance(), StorageAdmission::Result::Ready);
        QCOMPARE(admission->resume(), StorageAdmission::Result::Ready);
        QVERIFY(settings.attachStorageAdmission(admission)); // Never refresh generation.
        QVERIFY(!settings.attachStorageAdmission(std::make_shared<StorageAdmission>()));
        late();
        QCOMPARE(settings.storageErrorCode(), QString("StaleStorageGeneration"));
        QCOMPARE(bytes(path), before);
        QCOMPARE(settings.themeName(), QString("Paper"));
    }
    void concurrentForeignThreadWritersAreRefused() {
        QTemporaryDir profile;
        AppSettings settings(encrypted(profile.filePath("settings.json")),
                             inMemoryTestCredentialStore());
        settings.setThemeName("Paper");
        auto a = std::async(std::launch::async, [&] { settings.setThemeName("Linen"); });
        auto b = std::async(std::launch::async, [&] { settings.setThemeName("Sage Light"); });
        a.get();
        b.get();
        QCOMPARE(settings.themeName(), QString("Paper"));
        QCOMPARE(settings.storageErrorCode(), QString("SettingsWrongThread"));
        settings.setThemeName("Linen");
        QVERIFY(settings.storageErrorCode().isEmpty());
    }
    void sharedMemoryShutdownAndGlobalUnsupported() {
        QTemporaryDir profile;
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(encrypted(profile.filePath("settings.json")),
                             inMemoryTestCredentialStore());
        settings.attachStorageAdmission(admission);
        SQLiteMemoryStore memory(profile.filePath("memory.sqlite"), admission);
        settings.setThemeName("Paper");
        memory.put("key", "before");
        auto writer = admission->admit(admission->snapshot().generation);
        auto wait = std::async(std::launch::async, [&] { return admission->prepareLocal(2s); });
        QTRY_COMPARE(admission->snapshot().state, StorageAdmission::State::Quiescing);
        settings.setThemeName("Linen");
        QCOMPARE(settings.storageErrorCode(), QString("StorageBusy"));
        admission->cancel();
        QCOMPARE(wait.get(), StorageAdmission::Result::Cancelled);
        writer = {};
        settings.setThemeName("Linen");
        QCOMPARE(settings.themeName(), QString("Linen"));
        admission->shutdown();
        settings.setThemeName("Paper");
        memory.put("key", "after");
        QCOMPARE(settings.storageErrorCode(), QString("StorageUnavailable"));
        QCOMPARE(memory.get("key"), QString("before"));
        QCOMPARE(admission->prepareApplication(), StorageAdmission::Result::Unsupported);
    }
    void modelPreferencesDoNotChangeCachedSelectionWhenRefused() {
        QTemporaryDir profile;
        auto admission = std::make_shared<StorageAdmission>();
        AppSettings settings(encrypted(profile.filePath("settings.json")),
                             inMemoryTestCredentialStore());
        settings.attachStorageAdmission(admission);
        ModelService models(&settings);
        const auto before = models.selectedModel();
        QSignalSpy changed(&models, &ModelService::selectedModelChanged);
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        models.setSelectedModelId("refused-model");
        QCOMPARE(models.selectedModel().modelId, before.modelId);
        QCOMPARE(changed.count(), 0);
    }
};
QTEST_GUILESS_MAIN(SettingsStorageCoordinationTest)
#include "test_settings_storage_coordination.moc"
