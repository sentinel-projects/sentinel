// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/BackupService.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>
using namespace sentinel::core;
namespace {
MemoryManagementContext access(QString session = "s1", QString workspace = "p1",
                               bool personal = false, bool legacy = false) {
    return {{session, workspace, {workspace}, personal}, legacy};
}
MemoryMutation create(MemoryScope scope, QString key = "orchid", QString value = "first") {
    MemoryMutation m;
    m.record.scope = scope;
    m.record.key = key;
    m.record.value = value;
    return m;
}
} // namespace
class MemoryManagementTest final : public QObject {
    Q_OBJECT
private slots:
    void stores_data() {
        QTest::addColumn<bool>("sqlite");
        QTest::newRow("sqlite") << true;
        QTest::newRow("in-memory") << false;
    }
    void stores() {
        QFETCH(bool, sqlite);
        QTemporaryDir dir;
        std::unique_ptr<IMemoryStore> store =
            sqlite ? std::unique_ptr<IMemoryStore>(new SQLiteMemoryStore(dir.filePath("memory.db")))
                   : std::unique_ptr<IMemoryStore>(new InMemoryStore);
        QVERIFY(store->isAvailable());
        auto own = access();
        auto made = store->mutateManagedMemory({create(MemoryScope::Session)}, own);
        QCOMPARE(made.status, MemoryManagementStatus::Ready);
        QCOMPARE(made.records.size(), 1);
        auto record = made.records.first();
        QVERIFY(!record.revision.isEmpty());
        QVERIFY(!QUuid(record.id).isNull());
        QCOMPARE(record.provenance, MemoryProvenance::UserProvided);
        const auto created = record.createdAtUtc;
        QCOMPARE(store->listManagedMemory({MemoryScope::Session}, access("s2")).records.size(), 0);
        QCOMPARE(
            store->listManagedMemory({MemoryScope::Session}, access("s1", "p2")).records.size(), 0);
        QCOMPARE(store->listManagedMemory({MemoryScope::Project}, {}).status,
                 MemoryManagementStatus::Unauthorized);
        QCOMPARE(store->mutateManagedMemory({create(MemoryScope::Project)}, own).status,
                 MemoryManagementStatus::Ready);
        QCOMPARE(store->listManagedMemory({MemoryScope::Project}, access("s2")).records.size(), 1);
        QCOMPARE(
            store->listManagedMemory({MemoryScope::Project}, access("s2", "p2")).records.size(), 0);
        QCOMPARE(store->mutateManagedMemory({create(MemoryScope::Personal)}, own).status,
                 MemoryManagementStatus::Unauthorized);
        QCOMPARE(
            store->mutateManagedMemory({create(MemoryScope::Personal)}, access("s1", "p1", true))
                .status,
            MemoryManagementStatus::Ready);
        QCOMPARE(store->listManagedMemory({MemoryScope::Personal}, own).status,
                 MemoryManagementStatus::Unauthorized);
        QCOMPARE(store->retrieveMemory("orchid", own.access, 50).records.size(), 2);
        // Exercise correction and archive/delete authority for each non-Session scope too.
        for (const auto scope : {MemoryScope::Project, MemoryScope::Personal}) {
            const auto allowed = access("s1", "p1", scope == MemoryScope::Personal);
            auto scopedRecord = store->listManagedMemory({scope}, allowed).records.first();
            MemoryMutation scopedEdit;
            scopedEdit.kind = MemoryMutation::Kind::Correct;
            scopedEdit.record = scopedRecord;
            scopedEdit.record.value = "managed update";
            scopedEdit.expectedRevision = scopedRecord.revision;
            const auto changed = store->mutateManagedMemory({scopedEdit}, allowed);
            QCOMPARE(changed.status, MemoryManagementStatus::Ready);
            QCOMPARE(changed.records.first().createdAtUtc, scopedRecord.createdAtUtc);
            const auto archive = BackupService::exportScopedMemory(*store, scope, allowed);
            QVERIFY(archive.succeeded());
            SQLiteMemoryStore restored(
                dir.filePath(QString::number(static_cast<int>(scope)) + ".db"));
            QVERIFY(BackupService::importScopedMemory(restored, scope, allowed, archive.data, false)
                        .succeeded());
            auto imported = restored.readManagedMemory(scopedRecord.id, allowed).records.first();
            QCOMPARE(imported.value, QString("managed update"));
            QCOMPARE(imported.scope, scope);
            QCOMPARE(imported.provenance, scopedRecord.provenance);
            MemoryMutation removeImported;
            removeImported.kind = MemoryMutation::Kind::Delete;
            removeImported.record = imported;
            removeImported.expectedRevision = imported.revision;
            QVERIFY(restored.mutateManagedMemory({removeImported}, access("s2", "p2")).status !=
                    MemoryManagementStatus::Ready);
            QCOMPARE(restored.mutateManagedMemory({removeImported}, allowed).status,
                     MemoryManagementStatus::Ready);
            QCOMPARE(restored.readManagedMemory(imported.id, allowed).status,
                     MemoryManagementStatus::NotFound);
        }
        MemoryMutation edit;
        edit.kind = MemoryMutation::Kind::Correct;
        edit.record = record;
        edit.record.value = "corrected";
        edit.expectedRevision = record.revision;
        const auto edited = store->mutateManagedMemory({edit}, own);
        QCOMPARE(edited.status, MemoryManagementStatus::Ready);
        QCOMPARE(edited.records.first().id, record.id);
        QCOMPARE(edited.records.first().createdAtUtc, created);
        QCOMPARE(edited.records.first().corrections.size(), 1);
        QVERIFY(edited.records.first().revision != record.revision);
        QCOMPARE(store->mutateManagedMemory({edit}, own).status, MemoryManagementStatus::Conflict);
        MemoryMutation foreign = edit;
        foreign.expectedRevision = edited.records.first().revision;
        QCOMPARE(store->mutateManagedMemory({foreign}, access("s2", "p2")).status,
                 MemoryManagementStatus::NotFound);
        MemoryMutation remove;
        remove.kind = MemoryMutation::Kind::Delete;
        remove.record = edited.records.first();
        remove.expectedRevision = remove.record.revision;
        QCOMPARE(store->mutateManagedMemory({create(MemoryScope::Session, "rollback"), edit}, own)
                     .status,
                 MemoryManagementStatus::Conflict);
        QCOMPARE(store->retrieveMemory("rollback", own.access).records.size(), 0);
        QCOMPARE(store->mutateManagedMemory({remove}, own).status, MemoryManagementStatus::Ready);
        QCOMPARE(store->readManagedMemory(record.id, own).status, MemoryManagementStatus::NotFound);
        QCOMPARE(store->retrieveMemory("corrected", own.access).records.size(), 0);
        store->put("legacy", "unclassified");
        QCOMPARE(store->listManagedMemory({MemoryScope::Legacy}, own).status,
                 MemoryManagementStatus::Unauthorized);
        auto legacy =
            store->listManagedMemory({MemoryScope::Legacy}, access("s1", "p1", false, true));
        QCOMPARE(legacy.records.size(), 1);
        const auto legacyAuthority = access("s1", "p1", false, true);
        const auto legacyArchive =
            BackupService::exportScopedMemory(*store, MemoryScope::Legacy, legacyAuthority);
        QVERIFY(legacyArchive.succeeded());
        SQLiteMemoryStore restoredLegacy(dir.filePath("restored-legacy.db"));
        QVERIFY(BackupService::importScopedMemory(restoredLegacy, MemoryScope::Legacy,
                                                  legacyAuthority, legacyArchive.data, false)
                    .succeeded());
        const auto legacyRoundTrip =
            restoredLegacy.readManagedMemory(legacy.records.first().id, legacyAuthority)
                .records.first();
        QCOMPARE(legacyRoundTrip.value, QString("unclassified"));
        QCOMPARE(legacyRoundTrip.provenance, MemoryProvenance::Unknown);
        QVERIFY(!legacyRoundTrip.createdAtUtc.isValid());
        QVERIFY(!legacy.records.first().createdAtUtc.isValid());
        QCOMPARE(store->retrieveMemory("unclassified", own.access).records.size(), 0);
        MemoryMutation move;
        move.kind = MemoryMutation::Kind::Move;
        move.record = legacy.records.first();
        move.expectedRevision = move.record.revision;
        move.destination = MemoryScope::Project;
        const auto moved = store->mutateManagedMemory({move}, access("s1", "p1", false, true));
        QCOMPARE(moved.status, MemoryManagementStatus::Ready);
        QCOMPARE(moved.records.first().id, legacy.records.first().id);
        QCOMPARE(moved.records.first().provenance, MemoryProvenance::Unknown);
        QCOMPARE(store->retrieveMemory("unclassified", own.access).records.size(), 1);
        QCOMPARE(store->clearManagedMemory(MemoryScope::Project, access("s1", "p2")).status,
                 MemoryManagementStatus::Ready);
        QCOMPARE(store->listManagedMemory({MemoryScope::Project}, own).records.size(), 2);
        QCOMPARE(store->clearManagedMemory(MemoryScope::Project, own).status,
                 MemoryManagementStatus::Ready);
        QCOMPARE(store->listManagedMemory({MemoryScope::Project}, own).records.size(), 0);
        QCOMPARE(store->listManagedMemory({MemoryScope::Personal}, access("s1", "p1", true))
                     .records.size(),
                 1);
    }
    void archivesAndTransactionalRecovery() {
        QTemporaryDir dir;
        SQLiteMemoryStore source(dir.filePath("source.db")), target(dir.filePath("target.db"));
        auto own = access();
        const auto created =
            source.mutateManagedMemory({create(MemoryScope::Project)}, own).records.first();
        auto exported = BackupService::exportScopedMemory(source, MemoryScope::Project, own);
        QVERIFY(exported.succeeded());
        QVERIFY(BackupService::importScopedMemory(target, MemoryScope::Project, own, exported.data,
                                                  false)
                    .succeeded());
        const auto restored = target.readManagedMemory(created.id, own).records.first();
        QCOMPARE(restored.id, created.id);
        QCOMPARE(restored.value, created.value);
        QCOMPARE(restored.createdAtUtc, created.createdAtUtc);
        QCOMPARE(restored.updatedAtUtc, created.updatedAtUtc);
        QVERIFY(isImportedMemory(restored));
        QVERIFY(!BackupService::importScopedMemory(target, MemoryScope::Project, own, exported.data,
                                                   false)
                     .succeeded());
        QVERIFY(BackupService::importScopedMemory(target, MemoryScope::Project, own, exported.data,
                                                  true)
                    .succeeded());
        QVERIFY(!BackupService::importScopedMemory(target, MemoryScope::Project, access("s1", "p2"),
                                                   exported.data, true)
                     .succeeded());
        auto corrupt = exported.data;
        corrupt[corrupt.size() / 2] = '!';
        QVERIFY(
            !BackupService::importScopedMemory(target, MemoryScope::Project, own, corrupt, false)
                 .succeeded());
        auto future = QJsonDocument::fromJson(exported.data).object();
        future["version"] = 999;
        QCOMPARE(decodeMemoryArchive(QJsonDocument(future).toJson()).status,
                 MemoryManagementStatus::InvalidInput);
        QCOMPARE(decodeMemoryArchive(encodeMemoryArchive({created, created})).status,
                 MemoryManagementStatus::InvalidInput);
        auto malformed = created;
        malformed.ownerId = "foreign";
        QVERIFY(!BackupService::importScopedMemory(target, MemoryScope::Project, own,
                                                   encodeMemoryArchive({malformed}), false)
                     .succeeded());
        malformed = created;
        malformed.provenance = static_cast<MemoryProvenance>(200);
        QCOMPARE(decodeMemoryArchive(encodeMemoryArchive({malformed})).status,
                 MemoryManagementStatus::InvalidInput);
        auto extra = created;
        extra.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        extra.key = "would-roll-back";
        QVERIFY(!BackupService::importScopedMemory(target, MemoryScope::Project, own,
                                                   encodeMemoryArchive({extra, created}), false)
                     .succeeded());
        QCOMPARE(target.listManagedMemory({MemoryScope::Project}, own).records.size(), 1);
        SQLiteMemoryStore other(dir.filePath("target.db"));
        MemoryMutation edit;
        edit.kind = MemoryMutation::Kind::Correct;
        edit.record = restored;
        edit.record.value = "new";
        edit.expectedRevision = restored.revision;
        QCOMPARE(other.mutateManagedMemory({edit}, own).status, MemoryManagementStatus::Ready);
        QCOMPARE(target.mutateManagedMemory({edit}, own).status, MemoryManagementStatus::Conflict);
    }
    void corruptedPersistedMetadataFailsClosed() {
        QTemporaryDir dir;
        const auto path = dir.filePath("corrupt.db");
        SQLiteMemoryStore store(path);
        const auto made =
            store.mutateManagedMemory({create(MemoryScope::Project)}, access()).records.first();
        const auto connection = QUuid::createUuid().toString();
        {
            auto database = QSqlDatabase::addDatabase("QSQLITE", connection);
            database.setDatabaseName(path);
            QVERIFY(database.open());
            QSqlQuery query(database);
            QVERIFY(query.exec("UPDATE scoped_memory_entries SET corrections='{broken'"));
            database.close();
        }
        QSqlDatabase::removeDatabase(connection);
        QCOMPARE(store.listManagedMemory({MemoryScope::Project}, access()).status,
                 MemoryManagementStatus::InvalidInput);
        QCOMPARE(store.readManagedMemory(made.id, access()).status,
                 MemoryManagementStatus::InvalidInput);
        MemoryMutation edit;
        edit.kind = MemoryMutation::Kind::Correct;
        edit.record = made;
        edit.expectedRevision = made.revision;
        QCOMPARE(store.mutateManagedMemory({edit}, access()).status,
                 MemoryManagementStatus::InvalidInput);
        QVERIFY(
            !BackupService::exportScopedMemory(store, MemoryScope::Project, access()).succeeded());
    }
    void paginationAndSchemaCompatibility() {
        QTemporaryDir dir;
        const auto path = dir.filePath("memory.db");
        {
            SQLiteMemoryStore store(path);
            store.put("legacy", "old");
            for (int i = 0; i < 15; ++i) {
                QCOMPARE(store
                             .mutateManagedMemory(
                                 {create(MemoryScope::Project, QString::number(i))}, access())
                             .status,
                         MemoryManagementStatus::Ready);
            }
            auto page = store.listManagedMemory({MemoryScope::Project, {}, {}, 10}, access());
            QCOMPARE(page.records.size(), 10);
            QVERIFY(!page.nextCursor.isEmpty());
            auto next =
                store.listManagedMemory({MemoryScope::Project, {}, page.nextCursor, 10}, access());
            QCOMPARE(next.records.size(), 5);
            QVERIFY(next.nextCursor.isEmpty());
            QCOMPARE(store.schemaVersion(), 5);
        }
        SQLiteMemoryStore reopened(path);
        QCOMPARE(reopened.listManagedMemory({MemoryScope::Project}, access()).records.size(), 10);
        QCOMPARE(reopened.listManagedMemory({MemoryScope::Legacy}, access("s1", "p1", false, true))
                     .records.first()
                     .value,
                 QString("old"));
    }
};
QTEST_GUILESS_MAIN(MemoryManagementTest)
#include "test_memory_management.moc"
