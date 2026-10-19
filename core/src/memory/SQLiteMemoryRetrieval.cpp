// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
namespace sentinel::core {
namespace {
QString authority() {
    return "((m.scope=1 AND m.owner_id=? AND m.workspace_id=?) OR "
           "(m.scope=2 AND m.owner_id=? AND m.workspace_id=? AND ?<>'') OR "
           "(m.scope=3 AND m.owner_id='' AND m.workspace_id='' AND ?=1)) "
           "AND (?<0 OR m.scope=?)";
}
void bindAuthority(QSqlQuery& q, const MemoryAccessContext& c) {
    const auto workspace = c.workspaceId.isNull() ? QStringLiteral("") : c.workspaceId;
    q.addBindValue(c.sessionId);
    for (int i = 0; i < 4; ++i) {
        q.addBindValue(workspace);
    }
    q.addBindValue(c.includePersonal ? 1 : 0);
    const int scope = c.retrievalScope ? static_cast<int>(*c.retrievalScope) : -1;
    q.addBindValue(scope);
    q.addBindValue(scope);
}
QString consistent() {
    return "f.rowid=m.rowid AND f.id=m.id AND f.scope=m.scope AND f.owner=m.owner_id "
           "AND f.workspace=m.workspace_id AND f.key=m.key AND f.value=m.value AND "
           "f.revision=m.revision";
}
QStringList triggers() {
    const QString insert =
        "INSERT INTO scoped_memory_search(rowid,id,scope,owner,workspace,key,value,revision) "
        "VALUES(new.rowid,new.id,new.scope,new.owner_id,new.workspace_id,new.key,new.value,new."
        "revision);";
    return {"CREATE TRIGGER scoped_search_insert AFTER INSERT ON scoped_memory_entries BEGIN " +
                insert + " END",
            "CREATE TRIGGER scoped_search_delete AFTER DELETE ON scoped_memory_entries BEGIN "
            "DELETE FROM scoped_memory_search WHERE rowid=old.rowid; END",
            "CREATE TRIGGER scoped_search_update AFTER UPDATE ON scoped_memory_entries BEGIN "
            "DELETE FROM scoped_memory_search WHERE rowid=old.rowid; " +
                insert + " END"};
}
bool hooksReady(QSqlDatabase& db) {
    QSqlQuery q(db);
    for (const auto& sql : triggers()) {
        const auto name = sql.section(' ', 2, 2);
        q.prepare("SELECT sql FROM sqlite_master WHERE type='trigger' AND name=?");
        q.addBindValue(name);
        if (!q.exec() || !q.next() || q.value(0).toString() != sql) {
            return false;
        }
        q.finish();
    }
    return true;
}
QString orphanAuthority() {
    auto text = authority();
    text.replace("m.owner_id", "m.owner");
    text.replace("m.workspace_id", "m.workspace");
    return text;
}
} // namespace
void SQLiteMemoryStore::initializeRetrievalIndex() {
    QSqlQuery q(database_);
    // The optional FTS projection never owns authoritative memory. No full-table backfill here.
    if (!q.exec("CREATE VIRTUAL TABLE IF NOT EXISTS scoped_memory_search USING fts5("
                "id UNINDEXED,scope UNINDEXED,owner UNINDEXED,workspace "
                "UNINDEXED,key,value,revision UNINDEXED)")) {
        return;
    }
    if (!hooksReady(database_)) {
        for (const auto& sql : triggers()) {
            if (!q.exec("DROP TRIGGER IF EXISTS " + sql.section(' ', 2, 2)) || !q.exec(sql)) {
                return;
            }
        }
    }
}
MemoryIndexStatus SQLiteMemoryStore::indexStatus(QSqlDatabase& db, const MemoryAccessContext& c) {
    if (!authorizedMemoryContext(c)) {
        return {};
    }
    QSqlQuery q(db);
    // Read-only module/postings probe: an unusable FTS object is not advertised as Ready.
    q.prepare("SELECT rowid FROM scoped_memory_search WHERE scoped_memory_search MATCH ? LIMIT 1");
    q.addBindValue(QStringLiteral("\"sentinel_index_health_probe\""));
    if (!q.exec()) {
        return {};
    }
    q.finish();
    q.prepare("SELECT 1 FROM scoped_memory_entries m WHERE " + authority() +
              " AND NOT EXISTS (SELECT 1 FROM scoped_memory_search f WHERE " + consistent() +
              ") LIMIT 1");
    bindAuthority(q, c);
    if (!q.exec()) {
        return {};
    }
    if (q.next()) {
        return {MemoryIndexState::Stale};
    }
    q.finish();
    q.prepare("SELECT 1 FROM scoped_memory_search m WHERE " + orphanAuthority() +
              " AND NOT EXISTS (SELECT 1 FROM scoped_memory_entries b WHERE b.rowid=m.rowid AND "
              "b.id=m.id "
              "AND b.scope=m.scope AND b.owner_id=m.owner AND b.workspace_id=m.workspace) LIMIT 1");
    bindAuthority(q, c);
    if (!q.exec()) {
        return {};
    }
    if (q.next() || !hooksReady(db)) {
        return {MemoryIndexState::Stale};
    }
    return {MemoryIndexState::Ready};
}
MemoryIndexStatus SQLiteMemoryStore::memoryIndexStatus(const MemoryAccessContext& c) const {
    if (QThread::currentThread() != connectionThread_ || !isAvailable()) {
        return {};
    }
    auto db = database_;
    return indexStatus(db, c);
}
MemoryIndexStatus SQLiteMemoryStore::refreshMemoryIndex(const MemoryAccessContext& c, int limit) {
    if (!authorizedMemoryContext(c) || limit < 1 || limit > 100 ||
        QThread::currentThread() != connectionThread_ || !isAvailable() ||
        !database_.transaction()) {
        return {};
    }
    auto rollback = qScopeGuard([&] { database_.rollback(); });
    initializeRetrievalIndex();
    QSqlQuery q(database_);
    q.prepare("SELECT m.rowid FROM scoped_memory_entries m WHERE " + authority() +
              " AND NOT EXISTS (SELECT 1 FROM scoped_memory_search f WHERE " + consistent() +
              ") ORDER BY m.rowid LIMIT ?");
    bindAuthority(q, c);
    q.addBindValue(limit);
    if (!q.exec()) {
        return {};
    }
    QList<qint64> rows;
    while (q.next()) {
        rows.append(q.value(0).toLongLong());
    }
    q.finish();
    for (const auto rowid : rows) {
        q.prepare("DELETE FROM scoped_memory_search WHERE rowid=?");
        q.addBindValue(rowid);
        if (!q.exec()) {
            return {};
        }
        q.prepare(
            "INSERT INTO scoped_memory_search(rowid,id,scope,owner,workspace,key,value,revision) "
            "SELECT rowid,id,scope,owner_id,workspace_id,key,value,revision FROM "
            "scoped_memory_entries WHERE rowid=?");
        q.addBindValue(rowid);
        if (!q.exec()) {
            return {};
        }
        q.finish();
    }
    q.prepare(
        "DELETE FROM scoped_memory_search WHERE rowid IN (SELECT m.rowid FROM scoped_memory_search "
        "m WHERE " +
        orphanAuthority() +
        " AND NOT EXISTS (SELECT 1 FROM scoped_memory_entries b WHERE b.rowid=m.rowid AND "
        "b.id=m.id "
        "AND b.scope=m.scope AND b.owner_id=m.owner AND b.workspace_id=m.workspace) LIMIT ?)");
    bindAuthority(q, c);
    q.addBindValue(limit);
    if (!q.exec()) {
        return {};
    }
    const int removed = static_cast<int>(q.numRowsAffected());
    q.finish();
    auto result = indexStatus(database_, c);
    if (result.state == MemoryIndexState::Unavailable || !database_.commit()) {
        return {};
    }
    rollback.dismiss();
    result.refreshed = static_cast<int>(rows.size()) + removed;
    return result;
}
ScopedMemoryResult SQLiteMemoryStore::retrieveMemory(const QString& text,
                                                     const MemoryAccessContext& context,
                                                     int limit) const {
    if (!authorizedMemoryContext(context)) {
        return {ScopedMemoryStatus::Unauthorized, {}};
    }
    if (limit <= 0 || text.size() > 4096) {
        return {ScopedMemoryStatus::InvalidInput, {}};
    }
    if (!identityReady_) {
        return {};
    }
    QString connection;
    QSqlDatabase db;
    const auto cleanup = qScopeGuard([&] {
        db = {};
        if (!connection.isEmpty()) {
            QSqlDatabase::removeDatabase(connection);
        }
    });
    if (QThread::currentThread() == connectionThread_) {
        if (!isAvailable()) {
            return {};
        }
        db = database_;
    } else {
        if (databasePath_ == QLatin1String(":memory:")) {
            return {};
        }
        connection = "sentinel_scoped_read_" + QUuid::createUuid().toString(QUuid::Id128);
        db = QSqlDatabase::addDatabase("QSQLITE", connection);
        db.setDatabaseName(databasePath_);
        db.setConnectOptions("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=5000");
        if (!db.open()) {
            return {};
        }
    }
    const auto index = indexStatus(db, context);
    const auto terms = scopedMemoryQueryTerms(text);
    auto fetch = [&](bool indexed) {
        QSqlQuery q(db);
        QStringList matches;
        for (const auto& term : terms) {
            Q_UNUSED(term)
            matches.append("m.key LIKE ? ESCAPE '\\' OR m.value LIKE ? ESCAPE '\\'");
        }
        if (matches.isEmpty()) {
            matches.append("1=1");
        }
        QString match = matches.join(" OR ");
        if (indexed && !terms.isEmpty()) {
            match += " OR m.rowid IN (SELECT rowid FROM scoped_memory_search WHERE "
                     "scoped_memory_search MATCH ?)";
        }
        q.prepare(
            "SELECT m.id,m.scope,m.owner_id,m.workspace_id,m.key,m.value,m.provenance,m.source_ref,"
            "m.created_at,m.updated_at,m.revision,m.corrections FROM scoped_memory_entries m "
            "WHERE " +
            authority() + " AND (" + match +
            ") ORDER BY (lower(m.key)=lower(?)) DESC,"
            "(instr(lower(m.key),lower(?))>0) DESC,(instr(lower(m.value),lower(?))>0) DESC,"
            "m.updated_at DESC,m.id ASC LIMIT ?");
        bindAuthority(q, context);
        for (QString term : terms) {
            term.replace('\\', "\\\\");
            term.replace('%', "\\%");
            term.replace('_', "\\_");
            q.addBindValue("%" + term + "%");
            q.addBindValue("%" + term + "%");
        }
        if (indexed && !terms.isEmpty()) {
            QStringList tokens;
            for (auto term : terms) {
                term.replace('"', "\"\"");
                tokens.append("\"" + term + "\"");
            }
            q.addBindValue("{key value} : (" + tokens.join(" OR ") + ")");
        }
        for (int i = 0; i < 3; ++i) {
            q.addBindValue(text.simplified());
        }
        q.addBindValue(memoryCandidateLimit);
        ScopedMemoryResult result{ScopedMemoryStatus::Ready, {}};
        if (!q.exec()) {
            return ScopedMemoryResult{};
        }
        while (q.next()) {
            ScopedMemoryRecord record{
                q.value(0).toString(),
                static_cast<MemoryScope>(q.value(1).toInt()),
                q.value(2).toString(),
                q.value(3).toString(),
                q.value(4).toString(),
                q.value(5).toString(),
                static_cast<MemoryProvenance>(q.value(6).toInt()),
                q.value(7).toString(),
                QDateTime::fromString(q.value(8).toString(), Qt::ISODateWithMs),
                QDateTime::fromString(q.value(9).toString(), Qt::ISODateWithMs)};
            record.revision = q.value(10).toString();
            const auto receipts = QJsonDocument::fromJson(q.value(11).toByteArray());
            record.corrections = receipts.array();
            auto metadata = managedMemoryJson(record);
            metadata.remove("revision");
            ScopedMemoryRecord validated;
            if (!receipts.isArray() || record.revision.isEmpty() ||
                !parseManagedMemory(metadata, &validated)) {
                return ScopedMemoryResult{ScopedMemoryStatus::InvalidInput, {}};
            }
            if (visibleMemory(record, context)) {
                result.records.append(record);
            }
        }
        if (q.lastError().isValid()) {
            return ScopedMemoryResult{};
        }
        return result;
    };
    auto candidates = fetch(index.state == MemoryIndexState::Ready);
    auto effective = index;
    if (candidates.status == ScopedMemoryStatus::Unavailable &&
        index.state == MemoryIndexState::Ready) {
        candidates = fetch(false);
        effective = {};
    }
    if (candidates.status != ScopedMemoryStatus::Ready) {
        return candidates;
    }
    return rankScopedMemory(std::move(candidates.records), text, limit, effective);
}

} // namespace sentinel::core
