// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
namespace sentinel::core {
MemoryArchiveResult SQLiteMemoryStore::exportArchiveSnapshot(const QString& path, QIODevice& out,
                                                             MemoryScope scope,
                                                             const MemoryManagementContext& context,
                                                             const MemoryArchiveLimits& limits,
                                                             const MemoryArchiveControl& control) {
    MemoryArchiveResult failed;
    failed.status = "Unavailable";
    if (!authorizedMemoryScope(scope, context) || !limits.valid()) {
        failed.status = "Unauthorized";
        return failed;
    }
    const auto name = "sentinel_memory_snapshot_" + QUuid::createUuid().toString(QUuid::Id128);
    auto db = QSqlDatabase::addDatabase("QSQLITE", name);
    auto cleanup = qScopeGuard([&] {
        db.close();
        db = {};
        QSqlDatabase::removeDatabase(name);
    });
    db.setDatabaseName(path);
    db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000");
    if (!db.open() || !db.transaction()) {
        return failed;
    }
    auto rollback = qScopeGuard([&] { db.rollback(); });
    QSqlQuery query(db);
    query.setForwardOnly(true);
    if (!query.exec("SELECT value FROM memory_schema_metadata WHERE key='schema_version'") ||
        !query.next() || query.value(0).toInt() != currentSchemaVersion) {
        return failed;
    }
    query.finish();
    const bool legacy = scope == MemoryScope::Legacy;
    query.prepare(legacy ? "SELECT "
                           "record_uuid,0,'','',key,value,0,'',created_at,updated_at,revision,"
                           "corrections FROM memory_entries ORDER BY record_uuid LIMIT ?"
                         : "SELECT "
                           "id,scope,owner_id,workspace_id,key,value,provenance,source_ref,created_"
                           "at,updated_at,revision,corrections FROM scoped_memory_entries WHERE "
                           "scope=? AND owner_id=? AND workspace_id=? ORDER BY id LIMIT ?");
    if (!legacy) {
        query.addBindValue(static_cast<int>(scope));
        const auto owner = memoryOwner(scope, context.access),
                   workspace = memoryWorkspace(scope, context.access);
        query.addBindValue(owner.isNull() ? QStringLiteral("") : owner);
        query.addBindValue(workspace.isNull() ? QStringLiteral("") : workspace);
    }
    query.addBindValue(limits.maximumRecords + 1);
    if (!query.exec()) {
        return failed;
    }
    return writeMemoryArchive(
        out, scope, context, limits, control, [&](const MemoryArchiveConsumer& consume) {
            while (query.next()) {
                ScopedMemoryRecord r{
                    query.value(0).toString(),
                    static_cast<MemoryScope>(query.value(1).toInt()),
                    query.value(2).toString(),
                    query.value(3).toString(),
                    query.value(4).toString(),
                    query.value(5).toString(),
                    static_cast<MemoryProvenance>(query.value(6).toInt()),
                    query.value(7).toString(),
                    QDateTime::fromString(query.value(8).toString(), Qt::ISODateWithMs),
                    QDateTime::fromString(query.value(9).toString(), Qt::ISODateWithMs)};
                r.revision = query.value(10).toString();
                const auto receipts = QJsonDocument::fromJson(query.value(11).toByteArray());
                if (!receipts.isArray()) {
                    return QString("InvalidInput");
                }
                r.corrections = receipts.array();
                const auto status = consume(r);
                if (status != "Ready") {
                    return status;
                }
            }
            return query.lastError().isValid() ? QString("ReadFailure") : QString("Ready");
        });
}
MemoryArchiveResult SQLiteMemoryStore::restoreArchive(QIODevice& in, MemoryScope scope,
                                                      const MemoryManagementContext& context,
                                                      const MemoryArchiveLimits& limits,
                                                      const MemoryArchiveControl& control,
                                                      bool skipIdentical) {
    MemoryArchiveResult result;
    result.status = "Unavailable";
    if (QThread::currentThread() != connectionThread_ || !isAvailable() ||
        !authorizedMemoryScope(scope, context)) {
        return result;
    }
    QSqlQuery begin(database_);
    if (!begin.exec("PRAGMA busy_timeout=1000") || !begin.exec("PRAGMA synchronous=FULL") ||
        !begin.exec("BEGIN IMMEDIATE")) {
        result.status = "Busy";
        return result;
    }
    auto rollback = qScopeGuard([&] { database_.rollback(); });
    result = readMemoryArchive(
        in, scope, context, limits, control, [&](const ScopedMemoryRecord& record) {
            MemoryMutation change;
            change.kind = MemoryMutation::Kind::Import;
            change.record = record;
            change.skipIdentical = skipIdentical;
            return memoryManagementStatusName(applyManagedMutations({change}, context).status);
        });
    if (!result.ok()) {
        return result;
    }
    auto access = context.access;
    access.retrievalScope = scope;
    if (scope != MemoryScope::Legacy &&
        indexStatus(database_, access).state != MemoryIndexState::Ready) {
        result.status = "IndexNotReady";
        return result;
    }
    if ((control.cancelled && control.cancelled()) ||
        (control.beforeCommit && !control.beforeCommit())) {
        result.status = "Cancelled";
        return result;
    }
    if (!database_.commit()) {
        result.status = "CommitFailure";
        return result;
    }
    rollback.dismiss();
    return result;
}
} // namespace sentinel::core
