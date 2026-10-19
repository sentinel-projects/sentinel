// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
namespace sentinel::core {
namespace {
QString filter(const MemoryManagementContext& c, QSqlQuery& q) {
    Q_UNUSED(q)
    Q_UNUSED(c)
    return "((scope=1 AND owner_id=? AND workspace_id=?) OR (scope=2 AND owner_id=? AND "
           "workspace_id=? AND ?<>'') OR (scope=3 AND owner_id='' AND workspace_id='' AND ?=1))";
}
QString nonnull(const QString& s) {
    return s.isNull() ? QStringLiteral("") : s;
}
void bindContext(QSqlQuery& q, const MemoryManagementContext& c) {
    q.addBindValue(c.access.sessionId);
    q.addBindValue(nonnull(c.access.workspaceId));
    q.addBindValue(nonnull(c.access.workspaceId));
    q.addBindValue(nonnull(c.access.workspaceId));
    q.addBindValue(nonnull(c.access.workspaceId));
    q.addBindValue(c.access.includePersonal ? 1 : 0);
}
QString columns(bool legacy) {
    return legacy ? "record_uuid,0,'','',key,value,0,'',created_at,updated_at,revision,corrections"
                  : "id,scope,owner_id,workspace_id,key,value,provenance,source_ref,created_at,"
                    "updated_at,revision,corrections";
}
ScopedMemoryRecord row(QSqlQuery& q) {
    ScopedMemoryRecord r{q.value(0).toString(),
                         static_cast<MemoryScope>(q.value(1).toInt()),
                         q.value(2).toString(),
                         q.value(3).toString(),
                         q.value(4).toString(),
                         q.value(5).toString(),
                         static_cast<MemoryProvenance>(q.value(6).toInt()),
                         q.value(7).toString(),
                         QDateTime::fromString(q.value(8).toString(), Qt::ISODateWithMs),
                         QDateTime::fromString(q.value(9).toString(), Qt::ISODateWithMs)};
    r.revision = q.value(10).toString();
    const auto receipts = QJsonDocument::fromJson(q.value(11).toByteArray());
    r.corrections = receipts.array();
    auto metadata = managedMemoryJson(r);
    metadata.remove("revision");
    ScopedMemoryRecord validated;
    if (!receipts.isArray() || r.revision.isEmpty() || !parseManagedMemory(metadata, &validated)) {
        r.id.clear(); // Corrupt persisted metadata must never be silently rewritten.
    }
    return r;
}
} // namespace
MemoryManagementResult
SQLiteMemoryStore::listManagedMemory(const MemoryPageQuery& page,
                                     const MemoryManagementContext& c) const {
    if (!authorizedMemoryScope(page.scope, c)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    if (page.limit < 1 || page.limit > 50 || page.search.size() > 256 || page.afterId.size() > 64) {
        return {MemoryManagementStatus::InvalidInput, {}, {}};
    }
    if (QThread::currentThread() != connectionThread_ || !isAvailable()) {
        return {};
    }
    const bool legacy = page.scope == MemoryScope::Legacy;
    QSqlQuery q(database_);
    QString match = page.search;
    match.replace('\\', "\\\\");
    match.replace('%', "\\%");
    match.replace('_', "\\_");
    q.prepare("SELECT " + columns(legacy) + " FROM " +
              (legacy ? "memory_entries" : "scoped_memory_entries") + " WHERE " +
              (legacy ? QStringLiteral("1=1") : filter(c, q)) + " AND " +
              (legacy ? "record_uuid" : "id") + ">?" + (legacy ? "" : " AND scope=?") +
              " AND (key LIKE ? ESCAPE '\\' OR value LIKE ? ESCAPE '\\') ORDER BY " +
              (legacy ? "record_uuid" : "id") + " LIMIT ?");
    if (!legacy) {
        bindContext(q, c);
    }
    q.addBindValue(nonnull(page.afterId));
    if (!legacy) {
        q.addBindValue(static_cast<int>(page.scope));
    }
    q.addBindValue("%" + match + "%");
    q.addBindValue("%" + match + "%");
    q.addBindValue(page.limit + 1);
    if (!q.exec()) {
        return {};
    }
    MemoryManagementResult result{MemoryManagementStatus::Ready, {}, {}};
    while (q.next()) {
        if (result.records.size() == page.limit) {
            result.nextCursor = result.records.last().id;
            break;
        }
        auto record = row(q);
        if (record.id.isEmpty()) {
            return {MemoryManagementStatus::InvalidInput, {}, {}};
        }
        result.records.append(record);
    }
    return result;
}
MemoryManagementResult
SQLiteMemoryStore::readManagedMemory(const QString& id, const MemoryManagementContext& c) const {
    if (!authorizedMemoryContext(c.access)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    if (id.isEmpty() || id.size() > 64) {
        return {MemoryManagementStatus::InvalidInput, {}, {}};
    }
    if (QThread::currentThread() != connectionThread_ || !isAvailable()) {
        return {};
    }
    QSqlQuery q(database_);
    q.prepare("SELECT " + columns(false) + " FROM scoped_memory_entries WHERE id=? AND " +
              filter(c, q));
    q.addBindValue(id);
    bindContext(q, c);
    if (!q.exec()) {
        return {};
    }
    if (q.next()) {
        auto record = row(q);
        if (record.id.isEmpty()) {
            return {MemoryManagementStatus::InvalidInput, {}, {}};
        }
        return {MemoryManagementStatus::Ready, {record}, {}};
    }
    q.finish();
    if (c.allowLegacy) {
        q.prepare("SELECT " + columns(true) + " FROM memory_entries WHERE record_uuid=?");
        q.addBindValue(id);
        if (!q.exec()) {
            return {};
        }
        if (q.next()) {
            auto record = row(q);
            if (record.id.isEmpty()) {
                return {MemoryManagementStatus::InvalidInput, {}, {}};
            }
            return {MemoryManagementStatus::Ready, {record}, {}};
        }
    }
    return {MemoryManagementStatus::NotFound, {}, {}};
}
MemoryManagementResult
SQLiteMemoryStore::mutateManagedMemory(const QList<MemoryMutation>& mutations,
                                       const MemoryManagementContext& c) {
    if (!authorizedMemoryContext(c.access)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    if (mutations.isEmpty() || mutations.size() > 500) {
        return {MemoryManagementStatus::InvalidInput, {}, {}};
    }
    if (QThread::currentThread() != connectionThread_ || !isAvailable() ||
        !database_.transaction()) {
        return {};
    }
    auto rollback = qScopeGuard([&] { database_.rollback(); });
    auto result = applyManagedMutations(mutations, c);
    if (result.status != MemoryManagementStatus::Ready || !database_.commit()) {
        return result.status == MemoryManagementStatus::Ready ? MemoryManagementResult{} : result;
    }
    rollback.dismiss();
    return result;
}
MemoryManagementResult
SQLiteMemoryStore::applyManagedMutations(const QList<MemoryMutation>& mutations,
                                         const MemoryManagementContext& c) {
    MemoryManagementResult result{MemoryManagementStatus::Ready, {}, {}};
    for (const auto& m : mutations) {
        auto found = m.record.id.isEmpty()
                         ? MemoryManagementResult{MemoryManagementStatus::NotFound, {}, {}}
                         : readManagedMemory(m.record.id, c);
        if (found.status != MemoryManagementStatus::Ready &&
            found.status != MemoryManagementStatus::NotFound) {
            return found;
        }
        const auto* current = found.records.isEmpty() ? nullptr : &found.records.first();
        ScopedMemoryRecord next;
        bool skip = false;
        const auto status = prepareManagedMutation(m, current, c, &next, &skip);
        if (status != MemoryManagementStatus::Ready) {
            return {status, {}, {}};
        }
        if (skip) {
            continue;
        }
        QSqlQuery q(database_);
        QVariant legacyId;
        if (current && current->scope == MemoryScope::Legacy) {
            q.prepare("SELECT id FROM memory_entries WHERE record_uuid=?");
            q.addBindValue(current->id);
            if (!q.exec() || !q.next()) {
                return {};
            }
            legacyId = q.value(0);
            q.finish();
        }
        if (current) {
            q.prepare(current->scope == MemoryScope::Legacy
                          ? "DELETE FROM memory_entries WHERE record_uuid=? AND revision=?"
                          : "DELETE FROM scoped_memory_entries WHERE id=? AND revision=?");
            q.addBindValue(current->id);
            q.addBindValue(m.expectedRevision.isEmpty() ? current->revision : m.expectedRevision);
            if (!q.exec() || q.numRowsAffected() != 1) {
                return {MemoryManagementStatus::Conflict, {}, {}};
            }
        }
        if (m.kind == MemoryMutation::Kind::Delete) {
            continue;
        }
        q.finish();
        q.prepare("SELECT 1 FROM scoped_memory_entries WHERE id=? UNION ALL SELECT 1 FROM "
                  "memory_entries WHERE record_uuid=? LIMIT 1");
        q.addBindValue(next.id);
        q.addBindValue(next.id);
        if (!q.exec()) {
            return {};
        }
        if (q.next()) {
            return {MemoryManagementStatus::Conflict, {}, {}};
        }
        q.finish();
        if (next.scope == MemoryScope::Legacy) {
            q.prepare("INSERT INTO "
                      "memory_entries(id,record_uuid,key,value,created_at,updated_at,revision,"
                      "corrections) VALUES(?,?,?,?,?,?,?,?)");
            q.addBindValue(legacyId);
            q.addBindValue(next.id);
            q.addBindValue(next.key);
            q.addBindValue(next.value);
        } else {
            q.prepare(
                "INSERT INTO "
                "scoped_memory_entries(id,scope,owner_id,workspace_id,key,value,provenance,source_"
                "ref,created_at,updated_at,revision,corrections) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
            q.addBindValue(next.id);
            q.addBindValue(static_cast<int>(next.scope));
            q.addBindValue(nonnull(next.ownerId));
            q.addBindValue(nonnull(next.workspaceId));
            q.addBindValue(next.key);
            q.addBindValue(next.value);
            q.addBindValue(static_cast<int>(next.provenance));
            q.addBindValue(nonnull(next.sourceReference));
        }
        q.addBindValue(next.createdAtUtc.isValid()
                           ? QVariant(next.createdAtUtc.toString(Qt::ISODateWithMs))
                           : QVariant());
        q.addBindValue(next.updatedAtUtc.isValid()
                           ? QVariant(next.updatedAtUtc.toString(Qt::ISODateWithMs))
                           : QVariant());
        q.addBindValue(next.revision);
        q.addBindValue(
            QString::fromUtf8(QJsonDocument(next.corrections).toJson(QJsonDocument::Compact)));
        if (!q.exec()) {
            return {q.lastError().nativeErrorCode() == "19" ? MemoryManagementStatus::Conflict
                                                            : MemoryManagementStatus::Unavailable,
                    {},
                    {}};
        }
        result.records.append(next);
    }
    return result;
}

MemoryManagementResult SQLiteMemoryStore::clearManagedMemory(MemoryScope scope,
                                                             const MemoryManagementContext& c) {
    if (!authorizedMemoryScope(scope, c)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    if (QThread::currentThread() != connectionThread_ || !isAvailable() ||
        !database_.transaction()) {
        return {};
    }
    auto rollback = qScopeGuard([&] { database_.rollback(); });
    QSqlQuery q(database_);
    q.prepare(scope == MemoryScope::Legacy
                  ? QStringLiteral("DELETE FROM memory_entries")
                  : "DELETE FROM scoped_memory_entries WHERE scope=? AND " + filter(c, q));
    if (scope != MemoryScope::Legacy) {
        q.addBindValue(static_cast<int>(scope));
        bindContext(q, c);
    }
    if (!q.exec() || !database_.commit()) {
        return {};
    }
    rollback.dismiss();
    return {MemoryManagementStatus::Ready, {}, {}};
}
} // namespace sentinel::core
