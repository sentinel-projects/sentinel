// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/agent/ContextEngine.h"
#include "sentinel/core/app/BackupService.h"
#include "sentinel/core/context/ContextBudgetManager.h"
#include "sentinel/core/memory/InMemoryStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QJsonDocument>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTimeZone>
#include <QtTest>
using namespace sentinel::core;
namespace {
MemoryAccessContext own(bool personal = false) {
    return {"session", "project", {"project"}, personal};
}
ScopedMemoryRecord seed(IMemoryStore& store, MemoryScope scope, QString key, QString value,
                        MemoryAccessContext context = own()) {
    return store
        .writeScopedMemory(
            {scope, key, value, MemoryProvenance::GeneratedSummary, "summary:fixture"}, context)
        .records.first();
}
bool modifyDatabase(const QString& path, const QStringList& statements) {
    const auto name = QUuid::createUuid().toString();
    bool ok = true;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(path);
        ok = db.open();
        QSqlQuery q(db);
        for (const auto& sql : statements) {
            ok = q.exec(sql) && ok;
        }
        q.finish();
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}
} // namespace
class MemoryRetrievalTest final : public QObject {
    Q_OBJECT
private slots:
    void ranking_data() {
        QTest::addColumn<bool>("sqlite");
        QTest::newRow("sqlite") << true;
        QTest::newRow("direct") << false;
    }
    void ranking() {
        QFETCH(bool, sqlite);
        QTemporaryDir dir;
        std::unique_ptr<IMemoryStore> store =
            sqlite ? std::unique_ptr<IMemoryStore>(new SQLiteMemoryStore(dir.filePath("memory.db")))
                   : std::unique_ptr<IMemoryStore>(new InMemoryStore);
        const auto exact =
            seed(*store, MemoryScope::Project, "release checklist", "signed package policy");
        const auto phrase = seed(*store, MemoryScope::Session, "procedure",
                                 "Use the release checklist for delivery");
        seed(*store, MemoryScope::Session, "partial", "checklist only");
        seed(*store, MemoryScope::Session, "irrelevant", "gardening instructions");
        seed(*store, MemoryScope::Project, "foreign", "release checklist",
             {"other", "foreign", {"foreign"}, false});
        seed(*store, MemoryScope::Personal, "release checklist", "personal", own(true));
        store->put("release checklist", "legacy secret");
        auto result = store->retrieveMemory("release checklist", own(), 50);
        QCOMPARE(result.status, ScopedMemoryStatus::Ready);
        QCOMPARE(result.records.size(), 3);
        QCOMPARE(result.records.first().id, exact.id);
        QCOMPARE(result.records[1].id, phrase.id);
        QVERIFY(result.relevance.first().exactKey);
        QVERIFY(result.relevance[1].phrase);
        QCOMPARE(result.relevance.first().matchedTerms, 2);
        QCOMPARE(store->retrieveMemory("release checklist", own(true), 50).records.size(), 4);
        auto restricted = own();
        restricted.retrievalScope = MemoryScope::Project;
        QCOMPARE(store->retrieveMemory("release checklist", restricted, 50).records.size(), 1);
        QCOMPARE(store->retrieveMemory("release checklist", {}, 50).status,
                 ScopedMemoryStatus::Unauthorized);
        const auto repeat = store->retrieveMemory("release checklist", own(), 50);
        for (int i = 0; i < result.records.size(); ++i) {
            QCOMPARE(result.records[i].id, repeat.records[i].id);
        }
        // ContextEngine consumes ranked candidates rather than promoting Session scope above exact
        // Project hits.
        AgentContextInput input;
        input.goal = "release checklist";
        input.memoryStore = store.get();
        input.memoryAccess = own();
        QString firstMemory;
        for (const auto& item : ContextEngine{}.build(input).items) {
            if (item.kind == AgentContextKind::Memory) {
                firstMemory = item.source;
                break;
            }
        }
        QCOMPARE(firstMemory, exact.id);
        ModelBinding binding;
        binding.providerId = "fixture";
        binding.modelId = "fixture";
        binding.contextWindowTokens = 4096;
        QList<ContextBudgetItem> items;
        int priority = 500;
        for (const auto& record : result.records) {
            items.append(ContextBudgetManager::memoryItem(record, own(), priority--));
        }
        items.append({"User: release checklist", ContextBudgetKind::Conversation, 0, true});
        const auto assembled =
            ContextBudgetManager{}.assemble(binding, {}, items, {}, {"session", "project"});
        QVERIFY(assembled.accepted());
        QVERIFY(assembled.prompt.contains(exact.id));
        QVERIFY(!assembled.prompt.contains("legacy secret"));
        QVERIFY(!assembled.prompt.contains("foreign"));
    }
    void deterministicTiesAndCandidateBound() {
        QList<ScopedMemoryRecord> records;
        for (const auto& id : {QString("b"), QString("a")}) {
            ScopedMemoryRecord r;
            r.id = id;
            r.scope = MemoryScope::Session;
            r.key = "query";
            r.value = "same";
            r.updatedAtUtc = QDateTime::fromMSecsSinceEpoch(1000, QTimeZone::UTC);
            records.append(r);
        }
        QCOMPARE(rankScopedMemory(records, "query", 2).records.first().id, QString("a"));
        InMemoryStore store;
        auto exact = seed(store, MemoryScope::Project, "needle", "old exact key");
        for (int i = 0; i < 205; ++i) {
            seed(store, MemoryScope::Session, QString("row%1").arg(i), "contains needle");
        }
        const auto result = store.retrieveMemory("needle", own(), 3);
        QCOMPARE(result.records.first().id, exact.id);
        QCOMPARE(result.candidatesExamined, 200);
        QVERIFY(result.candidateLimitReached);
    }
    void transactionalIndexLifecycleAndRestart() {
        QTemporaryDir dir;
        const auto path = dir.filePath("memory.db");
        QString id;
        {
            SQLiteMemoryStore store(path);
            QCOMPARE(store.schemaVersion(), 6);
            QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Ready);
            auto record = seed(store, MemoryScope::Session, "before", "orchid");
            id = record.id;
            MemoryMutation edit;
            edit.kind = MemoryMutation::Kind::Correct;
            edit.record = record;
            edit.record.key = "after";
            edit.record.value = "updated";
            edit.expectedRevision = record.revision;
            const auto corrected = store.mutateManagedMemory({edit}, {own(), false});
            QCOMPARE(corrected.status, MemoryManagementStatus::Ready);
            QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Ready);
            QCOMPARE(store.retrieveMemory("before", own()).records.size(), 0);
            QCOMPARE(store.retrieveMemory("after", own()).records.first().id, id);
            MemoryMutation move;
            move.kind = MemoryMutation::Kind::Move;
            move.record = corrected.records.first();
            move.expectedRevision = move.record.revision;
            move.destination = MemoryScope::Project;
            const auto moved = store.mutateManagedMemory({move}, {own(), false});
            QCOMPARE(moved.status, MemoryManagementStatus::Ready);
            auto project = own();
            project.sessionId = "other-session";
            QCOMPARE(store.retrieveMemory("after", project).records.first().id, id);
            const auto archive =
                BackupService::exportScopedMemory(store, MemoryScope::Project, {own(), false});
            QVERIFY(archive.succeeded());
            MemoryMutation remove;
            remove.kind = MemoryMutation::Kind::Delete;
            remove.record = moved.records.first();
            remove.expectedRevision = remove.record.revision;
            QCOMPARE(store.mutateManagedMemory({remove}, {own(), false}).status,
                     MemoryManagementStatus::Ready);
            QCOMPARE(store.retrieveMemory("after", own()).records.size(), 0);
            QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Ready);
            QVERIFY(BackupService::importScopedMemory(store, MemoryScope::Project, {own(), false},
                                                      archive.data, false)
                        .succeeded());
            QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Ready);
        }
        SQLiteMemoryStore reopened(path);
        QCOMPARE(reopened.retrieveMemory("after", own()).records.first().id, id);
        QVERIFY(isImportedMemory(reopened.retrieveMemory("after", own()).records.first()));
        QCOMPARE(reopened.memoryIndexStatus(own()).state, MemoryIndexState::Ready);
    }
    void schemaFiveMigrationPreservesData() {
        QTemporaryDir dir;
        const auto path = dir.filePath("old-schema.db");
        ScopedMemoryRecord before, legacy;
        {
            SQLiteMemoryStore store(path);
            before = seed(store, MemoryScope::Project, "migration", "original");
            MemoryMutation correction;
            correction.kind = MemoryMutation::Kind::Correct;
            correction.record = before;
            correction.record.value = "preserved";
            correction.expectedRevision = before.revision;
            before = store.mutateManagedMemory({correction}, {own(), false}).records.first();
            store.put("legacy", "excluded");
            legacy = store.listManagedMemory({MemoryScope::Legacy}, {own(), true}).records.first();
        }
        QVERIFY(modifyDatabase(
            path, {"DROP TRIGGER scoped_search_insert", "DROP TRIGGER scoped_search_delete",
                   "DROP TRIGGER scoped_search_update", "DROP TABLE scoped_memory_search",
                   "UPDATE memory_schema_metadata SET value=5 WHERE key='schema_version'"}));
        SQLiteMemoryStore migrated(path);
        QCOMPARE(migrated.schemaVersion(), 6);
        const auto after = migrated.readManagedMemory(before.id, {own(), false}).records.first();
        QCOMPARE(after.value, before.value);
        QCOMPARE(after.createdAtUtc, before.createdAtUtc);
        QCOMPARE(after.provenance, before.provenance);
        QCOMPARE(after.sourceReference, before.sourceReference);
        QCOMPARE(after.corrections, before.corrections);
        QCOMPARE(
            migrated.listManagedMemory({MemoryScope::Legacy}, {own(), true}).records.first().id,
            legacy.id);
        QCOMPARE(migrated.retrieveMemory("excluded", own()).records.size(), 0);
        QCOMPARE(migrated.memoryIndexStatus(own()).state, MemoryIndexState::Stale);
        QCOMPARE(migrated.refreshMemoryIndex(own(), 1).state, MemoryIndexState::Ready);
        QCOMPARE(migrated.retrieveMemory("migration", own()).records.first().id, before.id);
    }
    void staleCorruptMissingAndBoundedRecovery() {
        QTemporaryDir dir;
        const auto path = dir.filePath("memory.db");
        {
            SQLiteMemoryStore store(path);
            for (int i = 0; i < 6; ++i) {
                seed(store, MemoryScope::Project, QString("orchid%1").arg(i), "policy");
            }
            QVERIFY(modifyDatabase(path, {"DELETE FROM scoped_memory_search"}));
            QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Stale);
            const auto fallback = store.retrieveMemory("orchid", own());
            QCOMPARE(fallback.records.size(), 6);
            QCOMPARE(fallback.index.state, MemoryIndexState::Stale);
            auto refreshed = store.refreshMemoryIndex(own(), 2);
            QCOMPARE(refreshed.state, MemoryIndexState::Stale);
            QCOMPARE(refreshed.refreshed, 2);
        }
        // Restart resumes from actual missing revisions; there is no fragile in-process cursor.
        SQLiteMemoryStore store(path);
        QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Stale);
        QCOMPARE(store.refreshMemoryIndex(own(), 2).state, MemoryIndexState::Stale);
        QCOMPARE(store.refreshMemoryIndex(own(), 2).state, MemoryIndexState::Ready);
        QVERIFY(modifyDatabase(path, {"UPDATE scoped_memory_search SET value='corrupt'"}));
        QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Stale);
        QCOMPARE(store.retrieveMemory("policy", own()).records.size(), 6);
        QCOMPARE(store.refreshMemoryIndex(own()).state, MemoryIndexState::Ready);
        QVERIFY(modifyDatabase(path, {"DROP TABLE scoped_memory_search"}));
        QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Unavailable);
        QCOMPARE(store.retrieveMemory("policy", own()).records.size(), 6);
        QCOMPARE(store.refreshMemoryIndex(own()).state, MemoryIndexState::Ready);
        QVERIFY(modifyDatabase(path, {"DROP TRIGGER scoped_search_update"}));
        QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Stale);
        QCOMPARE(store.refreshMemoryIndex(own()).state, MemoryIndexState::Ready);
        auto foreign = own();
        foreign.workspaceId = "foreign";
        foreign.authorizedWorkspaceIds = {"foreign"};
        QVERIFY(modifyDatabase(path, {"DELETE FROM scoped_memory_search"}));
        QCOMPARE(store.refreshMemoryIndex(foreign).refreshed, 0);
        QCOMPARE(store.memoryIndexStatus(own()).state, MemoryIndexState::Stale);
        QCOMPARE(store.refreshMemoryIndex({}, 100).state, MemoryIndexState::Unavailable);
        QVERIFY(!BackupService::exportScopedMemory(store, MemoryScope::Personal, {own(), false})
                     .succeeded());
    }
};
QTEST_GUILESS_MAIN(MemoryRetrievalTest)
#include "test_memory_retrieval.moc"
