// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/agent/ContextEngine.h"
#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/app/BackupService.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtConcurrentRun>
#include <QtTest>
using namespace sentinel::core;
namespace {
MemoryAccessContext access(QString session = "s1", QString workspace = "p1",
                           bool personal = false) {
    return {session, workspace, {workspace}, personal};
}
} // namespace
class ScopedMemoryTest final : public QObject {
    Q_OBJECT
private slots:
    void scopes_data() {
        QTest::addColumn<bool>("sqlite");
        QTest::newRow("sqlite") << true;
        QTest::newRow("memory") << false;
    }
    void scopes() {
        QFETCH(bool, sqlite);
        QTemporaryDir dir;
        std::unique_ptr<IMemoryStore> store;
        if (sqlite)
            store = std::make_unique<SQLiteMemoryStore>(dir.filePath("memory.db"));
        else
            store = std::make_unique<InMemoryStore>();
        const auto session =
            store->writeScopedMemory({MemoryScope::Session, "orchid", "private session",
                                      MemoryProvenance::UserProvided, "conversation:s1"},
                                     access());
        QCOMPARE(session.status, ScopedMemoryStatus::Ready);
        QVERIFY(!session.records.first().id.isEmpty());
        ScopedMemoryWrite conflict{MemoryScope::Session, "orchid", "should not overwrite"};
        conflict.overwriteExisting = false;
        QCOMPARE(store->writeScopedMemory(conflict, access()).status,
                 ScopedMemoryStatus::InvalidInput);
        QCOMPARE(store->retrieveMemory("orchid", access()).records.first().value,
                 QString("private session"));
        QCOMPARE(store->retrieveMemory("orchid", access()).records.size(), 1);
        QVERIFY(store->retrieveMemory("orchid", access("s2")).records.isEmpty());
        QVERIFY(store->retrieveMemory("orchid", access("s1", "p2")).records.isEmpty());
        QCOMPARE(store
                     ->writeScopedMemory({MemoryScope::Project, "orchid", "project one",
                                          MemoryProvenance::ProjectDerived, "fixture:project"},
                                         access())
                     .status,
                 ScopedMemoryStatus::Ready);
        QCOMPARE(store->retrieveMemory("orchid", access("s2")).records.size(), 1);
        QVERIFY(store->retrieveMemory("orchid", access("s2", "p2")).records.isEmpty());
        auto forged = access();
        forged.workspaceId = "p2";
        QCOMPARE(store->retrieveMemory("orchid", forged).status, ScopedMemoryStatus::Unauthorized);
        QCOMPARE(
            store->writeScopedMemory({MemoryScope::Project, "orchid", "denied"}, forged).status,
            ScopedMemoryStatus::Unauthorized);
        QCOMPARE(store
                     ->writeScopedMemory({MemoryScope::Personal, "orchid", "explicit personal"},
                                         access())
                     .status,
                 ScopedMemoryStatus::Unauthorized);
        QCOMPARE(store
                     ->writeScopedMemory({MemoryScope::Personal, "orchid", "explicit personal"},
                                         access("s1", "p1", true))
                     .status,
                 ScopedMemoryStatus::Ready);
        QCOMPARE(store->retrieveMemory("orchid", access("s2", "p2", true)).records.size(), 1);
        QVERIFY(store->retrieveMemory("orchid", access("s2", "p2")).records.isEmpty());
        store->put("orchid", "legacy secret");
        const auto legacy = store->searchRelevantRecords("orchid", 10);
        QCOMPARE(legacy.size(), 1);
        QCOMPARE(legacy.first().scope, MemoryScope::Legacy);
        QCOMPARE(legacy.first().provenance, MemoryProvenance::Unknown);
        QCOMPARE(store->get("orchid"), QString("legacy secret"));
        for (const auto& r : store->retrieveMemory({}, access("s1", "p1", true), 50).records)
            QVERIFY(r.scope != MemoryScope::Legacy);
        QCOMPARE(store->retrieveMemory({}, access(), 1).records.size(), 1);
        QCOMPARE(store->retrieveMemory({}, {}).status, ScopedMemoryStatus::Unauthorized);
        QCOMPARE(store->retrieveMemory({}, access(), 0).status, ScopedMemoryStatus::InvalidInput);
        QCOMPARE(
            store->writeScopedMemory({MemoryScope::Session, "pattern", "liter_ally 50%"}, access())
                .status,
            ScopedMemoryStatus::Ready);
        QCOMPARE(store->retrieveMemory("liter_ally", access()).records.size(), 1);
        QCOMPARE(store->retrieveMemory("%", access()).records.size(), 1);
        store->clear();
        QVERIFY(store->entries().isEmpty());
        QVERIFY(store->retrieveMemory({}, access("s1", "p1", true)).records.isEmpty());
    }
    void agentContextUsesScopedProvenance() {
        InMemoryStore store;
        auto own = access();
        store.writeScopedMemory({MemoryScope::Project, "orchid", "SAFE_PROJECT",
                                 MemoryProvenance::GeneratedSummary, "summary:project"},
                                own);
        store.writeScopedMemory({MemoryScope::Session, "orchid", "OTHER_SESSION"}, access("s2"));
        store.writeScopedMemory({MemoryScope::Project, "orchid", "OTHER_PROJECT"},
                                access("s3", "p2"));
        store.put("orchid", "LEGACY_UNKNOWN");
        AgentContextInput input;
        input.goal = "Please recall orchid";
        input.memoryStore = &store;
        input.memoryAccess = own;
        input.contextWindowTokens = 8192;
        const auto context = ContextEngine{}.build(input);
        QString memory;
        for (const auto& item : context.items)
            if (item.kind == AgentContextKind::Memory) {
                memory += item.content;
                QVERIFY(item.untrusted);
                QVERIFY(!item.source.isEmpty());
            }
        QVERIFY(memory.contains("SAFE_PROJECT"));
        QVERIFY(memory.contains("generated-summary"));
        QVERIFY(!memory.contains("OTHER_SESSION"));
        QVERIFY(!memory.contains("OTHER_PROJECT"));
        QVERIFY(!memory.contains("LEGACY_UNKNOWN"));
        class DeniedStore final : public IMemoryStore {
        public:
            void put(QString, QString) override {}
            QString get(const QString&) const override {
                return {};
            }
            MemoryEntries entries() const override {
                return {};
            }
            void clear() override {}
            ScopedMemoryResult retrieveMemory(const QString&, const MemoryAccessContext&,
                                              int) const override {
                ScopedMemoryRecord record;
                record.id = "must-not-appear";
                record.value = "DENIED_PAYLOAD";
                return {ScopedMemoryStatus::Unauthorized, {record}};
            }
        } denied;
        input.memoryStore = &denied;
        for (const auto& item : ContextEngine{}.build(input).items)
            QVERIFY(item.kind != AgentContextKind::Memory);
        input.memoryStore = &store;
        input.memoryAccess = {};
        for (const auto& item : ContextEngine{}.build(input).items)
            QVERIFY(item.kind != AgentContextKind::Memory);
    }

    void legacyBackupCannotDiscardScopedData() {
        QTemporaryDir dir;
        AppSettings settings(std::make_unique<JsonSettingsStore>(dir.filePath("settings.json")),
                             inMemoryTestCredentialStore());
        InMemoryStore memory;
        BackupService backup(settings, nullptr, &memory);
        const auto legacy = backup.exportJson({"memory"});
        QVERIFY(legacy.succeeded());
        memory.put("old", "legacy");
        QVERIFY(!backup.exportJson({"memory"}).succeeded());
        QVERIFY(!backup.importJson(legacy.data, {"memory"}, ImportMode::ReplaceSelectedDomains)
                     .succeeded());
        QCOMPARE(
            memory.writeScopedMemory({MemoryScope::Session, "key", "retained"}, access()).status,
            ScopedMemoryStatus::Ready);
        QVERIFY(!backup.exportJson({"memory"}).succeeded());
        QVERIFY(!backup.importJson(legacy.data, {"memory"}, ImportMode::ReplaceSelectedDomains)
                     .succeeded());
        QCOMPARE(memory.retrieveMemory("retained", access()).records.size(), 1);
        QCOMPARE(memory.get("old"), QString("legacy"));
    }

    void migrationRefusesUnsafeSchemasWithoutDataLoss_data() {
        QTest::addColumn<bool>("future");
        QTest::newRow("future") << true;
        QTest::newRow("failed-migration") << false;
    }
    void migrationRefusesUnsafeSchemasWithoutDataLoss() {
        QFETCH(bool, future);
        QTemporaryDir dir;
        const auto path = dir.filePath("unsafe.db");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "p801-unsafe");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE memory_entries(id INTEGER PRIMARY KEY,key TEXT UNIQUE NOT "
                           "NULL,value TEXT NOT NULL)"));
            QVERIFY(q.exec("INSERT INTO memory_entries VALUES(42,'key','unchanged bytes')"));
            QVERIFY(
                q.exec("CREATE TABLE memory_schema_metadata(key TEXT PRIMARY KEY,value INTEGER)"));
            QVERIFY(q.exec(future
                               ? "INSERT INTO memory_schema_metadata VALUES('schema_version',99)"
                               : "INSERT INTO memory_schema_metadata VALUES('schema_version',3)"));
            if (!future)
                QVERIFY(q.exec("CREATE TABLE scoped_memory_entries(wrong_column TEXT)"));
        }
        QSqlDatabase::removeDatabase("p801-unsafe");
        {
            SQLiteMemoryStore store(path);
            QVERIFY(!store.isAvailable());
            QCOMPARE(store.retrieveMemory({}, access()).status, ScopedMemoryStatus::Unavailable);
        }
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "p801-check");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("SELECT id,value FROM memory_entries"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), 42);
            QCOMPARE(q.value(1).toString(), QString("unchanged bytes"));
            q.finish();
            QVERIFY(q.exec("SELECT value FROM memory_schema_metadata WHERE key='schema_version'"));
            QVERIFY(q.next());
            QCOMPARE(q.value(0).toInt(), future ? 99 : 3);
        }
        QSqlDatabase::removeDatabase("p801-check");
    }

    void persistenceAndMigration() {
        QTemporaryDir dir;
        const auto path = dir.filePath("legacy.db");
        {
            auto db = QSqlDatabase::addDatabase("QSQLITE", "p801-legacy");
            db.setDatabaseName(path);
            QVERIFY(db.open());
            QSqlQuery q(db);
            QVERIFY(q.exec("CREATE TABLE memory_entries(id INTEGER PRIMARY KEY,key TEXT UNIQUE NOT "
                           "NULL,value TEXT NOT NULL)"));
            QVERIFY(q.exec("INSERT INTO memory_entries VALUES(42,'orchid','legacy bytes')"));
            QVERIFY(
                q.exec("CREATE TABLE memory_schema_metadata(key TEXT PRIMARY KEY,value INTEGER)"));
            QVERIFY(q.exec("INSERT INTO memory_schema_metadata VALUES('schema_version',3)"));
        }
        QSqlDatabase::removeDatabase("p801-legacy");
        ScopedMemoryRecord saved;
        {
            SQLiteMemoryStore store(path);
            QVERIFY(store.isAvailable());
            QCOMPARE(store.schemaVersion(), 6);
            QCOMPARE(store.searchRelevantRecords("orchid", 10).first().id, qint64(42));
            QVERIFY(store.retrieveMemory({}, access("s1", "p1", true)).records.isEmpty());
            auto result =
                store.writeScopedMemory({MemoryScope::Session, "orchid", "summary claim",
                                         MemoryProvenance::GeneratedSummary, "summary:fixture"},
                                        access());
            QCOMPARE(result.status, ScopedMemoryStatus::Ready);
            saved = result.records.first();
            QVERIFY(saved.createdAtUtc.isValid());
        }
        {
            SQLiteMemoryStore store(path);
            QCOMPARE(store.get("orchid"), QString("legacy bytes"));
            auto rows = store.retrieveMemory("orchid", access()).records;
            QCOMPARE(rows.size(), 1);
            QCOMPARE(rows.first().id, saved.id);
            QCOMPARE(rows.first().createdAtUtc, saved.createdAtUtc);
            QCOMPARE(rows.first().provenance, MemoryProvenance::GeneratedSummary);
            QCOMPARE(rows.first().sourceReference, QString("summary:fixture"));
            auto updated =
                store.writeScopedMemory({MemoryScope::Session, "orchid", "corrected",
                                         MemoryProvenance::UserProvided, "user:correction"},
                                        access());
            QCOMPARE(updated.status, ScopedMemoryStatus::Ready);
            QCOMPARE(updated.records.first().id, saved.id);
            QCOMPARE(updated.records.first().createdAtUtc, saved.createdAtUtc);
            // Qt SQL read connection belongs to the requesting worker, not the UI thread.
            auto future =
                QtConcurrent::run([&] { return store.retrieveMemory("orchid", access()); });
            future.waitForFinished();
            QCOMPARE(future.result().status, ScopedMemoryStatus::Ready);
            QCOMPARE(future.result().records.first().id, saved.id);
            QVERIFY(scopedMemoryContextText({ScopedMemoryStatus::Ready, {saved}})
                        .contains("generated-summary"));
        }
    }
};
QTEST_GUILESS_MAIN(ScopedMemoryTest)
#include "test_scoped_memory.moc"
