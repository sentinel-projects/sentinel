// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/memory/SQLiteMemoryStore.h"

#include "sentinel/core/memory/SqlitePragmas.h"
#include "sentinel/core/memory/FtsQuery.h"

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <QVariant>
#include <algorithm>

namespace sentinel::core {

SQLiteMemoryStore::SQLiteMemoryStore(QString databasePath)
    : databasePath_(std::move(databasePath)),
      connectionName_(
          QStringLiteral("sentinel_memory_%1").arg(QUuid::createUuid().toString(QUuid::Id128))) {
    open();
    initializeSchema();
}

SQLiteMemoryStore::~SQLiteMemoryStore() {
    const auto connectionName = connectionName_;
    database_ = {};
    QSqlDatabase::removeDatabase(connectionName);
}

void SQLiteMemoryStore::put(QString key, QString value) {
    if (!isAvailable()) {
        setLastError(QStringLiteral("SQLite memory database is not open."));
        return;
    }

    QSqlQuery query(database_);
    if (key.isEmpty()) {
        query.prepare(QStringLiteral("INSERT INTO memory_entries(key, value) VALUES('', ?) "
                                     "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    } else {
        query.prepare(QStringLiteral("INSERT INTO memory_entries(key, value) VALUES(?, ?) "
                                     "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
        query.addBindValue(key);
    }
    query.addBindValue(value);
    if (!query.exec()) {
        setLastError(query.lastError().text());
    } else {
        setLastError({});
    }
}

QString SQLiteMemoryStore::get(const QString& key) const {
    if (!isAvailable()) {
        setLastError(QStringLiteral("SQLite memory database is not open."));
        return {};
    }

    QSqlQuery query(database_);
    if (key.isEmpty()) {
        query.prepare(QStringLiteral("SELECT value FROM memory_entries WHERE key = ''"));
    } else {
        query.prepare(QStringLiteral("SELECT value FROM memory_entries WHERE key = ?"));
        query.addBindValue(key);
    }

    if (!query.exec()) {
        setLastError(query.lastError().text());
        return {};
    }

    setLastError({});
    if (!query.next()) {
        return {};
    }

    return query.value(0).toString();
}

MemoryEntries SQLiteMemoryStore::entries() const {
    MemoryEntries result;
    if (!isAvailable()) {
        setLastError(QStringLiteral("SQLite memory database is not open."));
        return result;
    }

    QSqlQuery query(database_);

    if (!query.exec(QStringLiteral("SELECT key, value FROM memory_entries ORDER BY key ASC"))) {
        setLastError(query.lastError().text());
        return result;
    }

    setLastError({});
    while (query.next()) {
        result.append({query.value(0).toString(), query.value(1).toString()});
    }

    return result;
}

MemoryEntries SQLiteMemoryStore::searchRelevant(const QString& text, int limit) const {
    MemoryEntries result;
    for (const auto& record : searchRelevantRecords(text, limit))
        result.append({record.key, record.value});
    return result;
}

QList<MemoryRecord> SQLiteMemoryStore::searchRelevantRecords(const QString& text, int limit) const {
    QList<MemoryRecord> result;
    if (text.trimmed().isEmpty() || limit <= 0 || !isAvailable())
        return result;
    const QString match = ftsMatchQuery(text);
    if (ftsReady_ && !match.isEmpty()) {
        QSqlQuery indexed(database_);
        indexed.prepare(QStringLiteral(
            "SELECT m.id, m.key, m.value FROM memory_entries_fts "
            "JOIN memory_entries AS m ON m.id = memory_entries_fts.memory_id "
            "WHERE memory_entries_fts MATCH ? "
            "ORDER BY bm25(memory_entries_fts, 0.0, 4.0, 1.0), m.id DESC LIMIT ?"));
        indexed.addBindValue(match);
        indexed.addBindValue(qBound(1, limit, 50));
        if (indexed.exec()) {
            setLastError({});
            while (indexed.next())
                result.append({indexed.value(0).toLongLong(), indexed.value(1).toString(),
                               indexed.value(2).toString()});
            return result;
        }
        setLastError(indexed.lastError().text());
    }
    QStringList words = text.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    words.erase(std::remove_if(words.begin(), words.end(), [](const QString& word) {
        return word.size() < 3;
    }), words.end());
    words = words.mid(0, 5);
    if (words.isEmpty())
        words.append(text.trimmed().left(80));
    QStringList conditions;
    QStringList scores;
    for (const auto& word : words) {
        Q_UNUSED(word)
        conditions.append(QStringLiteral("(key LIKE ? ESCAPE '\\' OR value LIKE ? ESCAPE '\\')"));
        scores.append(QStringLiteral("(CASE WHEN key LIKE ? ESCAPE '\\' THEN 3 ELSE 0 END + "
                                     "CASE WHEN value LIKE ? ESCAPE '\\' THEN 1 ELSE 0 END)"));
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT id, key, value FROM memory_entries WHERE %1 ORDER BY %2 DESC, id DESC LIMIT ?")
                      .arg(conditions.join(QStringLiteral(" OR ")), scores.join(QLatin1Char('+'))));
    auto pattern = [](QString word) {
        word.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        word.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        word.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        return QLatin1Char('%') + word + QLatin1Char('%');
    };
    for (const auto& word : words) {
        query.addBindValue(pattern(word));
        query.addBindValue(pattern(word));
    }
    for (const auto& word : words) {
        query.addBindValue(pattern(word));
        query.addBindValue(pattern(word));
    }
    query.addBindValue(qBound(1, limit, 50));
    if (!query.exec()) {
        setLastError(query.lastError().text());
        return result;
    }
    setLastError({});
    while (query.next())
        result.append({query.value(0).toLongLong(), query.value(1).toString(),
                       query.value(2).toString()});
    return result;
}

ScopedMemoryResult SQLiteMemoryStore::writeScopedMemory(const ScopedMemoryWrite& write,
                                                        const MemoryAccessContext& context) {
    if (!permittedMemoryWrite(write, context)) {
        return {ScopedMemoryStatus::Unauthorized, {}};
    }
    if (!validMemoryWrite(write)) {
        return {ScopedMemoryStatus::InvalidInput, {}};
    }
    if (QThread::currentThread() != connectionThread_ || !isAvailable()) {
        return {};
    }
    if (!database_.transaction()) {
        return {};
    }
    auto rollback = qScopeGuard([&] {
        if (database_.isOpen()) {
            database_.rollback();
        }
    });
    const auto owner = memoryOwner(write.scope, context);
    const auto workspace = memoryWorkspace(write.scope, context);
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery q(database_);
    q.prepare(
        "INSERT INTO "
        "scoped_memory_entries(id,scope,owner_id,workspace_id,key,value,provenance,source_ref,"
        "created_at,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?) ON CONFLICT(scope,owner_id,workspace_id,key) DO UPDATE SET "
        "value=excluded.value,provenance=excluded.provenance,source_ref=excluded.source_ref,"
        "updated_at=excluded.updated_at WHERE ?=1");
    q.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces));
    q.addBindValue(static_cast<int>(write.scope));
    q.addBindValue(owner.isNull() ? QStringLiteral("") : owner);
    q.addBindValue(workspace.isNull() ? QStringLiteral("") : workspace);
    q.addBindValue(write.key);
    q.addBindValue(write.value);
    q.addBindValue(static_cast<int>(write.provenance));
    q.addBindValue(write.sourceReference.isNull() ? QStringLiteral("") : write.sourceReference);
    q.addBindValue(now);
    q.addBindValue(now);
    q.addBindValue(write.overwriteExisting ? 1 : 0);
    if (!q.exec()) {
        setLastError(QStringLiteral("Scoped memory write failed."));
        return {};
    }
    if (q.numRowsAffected() == 0) {
        return {ScopedMemoryStatus::InvalidInput, {}};
    }
    q.finish();
    q.prepare("SELECT id,created_at FROM scoped_memory_entries WHERE scope=? AND owner_id=? AND "
              "workspace_id=? AND key=?");
    q.addBindValue(static_cast<int>(write.scope));
    q.addBindValue(owner.isNull() ? QStringLiteral("") : owner);
    q.addBindValue(workspace.isNull() ? QStringLiteral("") : workspace);
    q.addBindValue(write.key);
    if (!q.exec() || !q.next()) {
        return {};
    }
    ScopedMemoryRecord record{q.value(0).toString(),
                              write.scope,
                              owner,
                              workspace,
                              write.key,
                              write.value,
                              write.provenance,
                              write.sourceReference,
                              QDateTime::fromString(q.value(1).toString(), Qt::ISODateWithMs),
                              QDateTime::fromString(now, Qt::ISODateWithMs)};
    q.finish();
    if (!database_.commit()) {
        return {};
    }
    rollback.dismiss();
    setLastError({});
    return {ScopedMemoryStatus::Ready, {record}};
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
    QSqlQuery q(db);
    const auto terms = scopedMemoryQueryTerms(text);
    QStringList matches;
    for (const auto& term : terms) {
        Q_UNUSED(term)
        matches.append(QStringLiteral("key LIKE ? ESCAPE '\\' OR value LIKE ? ESCAPE '\\'"));
    }
    if (matches.isEmpty()) {
        matches.append(QStringLiteral("1=1"));
    }
    q.prepare(
        QStringLiteral(
            "SELECT "
            "id,scope,owner_id,workspace_id,key,value,provenance,source_ref,created_at,updated_at "
            "FROM scoped_memory_entries WHERE ((scope=1 AND owner_id=? AND workspace_id=?) OR "
            "(scope=2 AND owner_id=? AND workspace_id=? AND ?<>'') OR "
            "(scope=3 AND owner_id='' AND workspace_id='' AND ?=1)) AND (%1) "
            "ORDER BY updated_at DESC,id ASC LIMIT ?")
            .arg(matches.join(QStringLiteral(" OR "))));
    const auto workspace = context.workspaceId.isNull() ? QStringLiteral("") : context.workspaceId;
    q.addBindValue(context.sessionId);
    q.addBindValue(workspace);
    q.addBindValue(workspace);
    q.addBindValue(workspace);
    q.addBindValue(workspace);
    q.addBindValue(context.includePersonal ? 1 : 0);
    for (QString term : terms) {
        term.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        term.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        term.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        const auto pattern = QLatin1Char('%') + term + QLatin1Char('%');
        q.addBindValue(pattern);
        q.addBindValue(pattern);
    }
    q.addBindValue(qBound(1, limit, 50));
    if (!q.exec()) {
        return {};
    }
    ScopedMemoryResult result{ScopedMemoryStatus::Ready, {}};
    while (q.next()) {
        ScopedMemoryRecord record{q.value(0).toString(),
                                  static_cast<MemoryScope>(q.value(1).toInt()),
                                  q.value(2).toString(),
                                  q.value(3).toString(),
                                  q.value(4).toString(),
                                  q.value(5).toString(),
                                  static_cast<MemoryProvenance>(q.value(6).toInt()),
                                  q.value(7).toString(),
                                  QDateTime::fromString(q.value(8).toString(), Qt::ISODateWithMs),
                                  QDateTime::fromString(q.value(9).toString(), Qt::ISODateWithMs)};
        if (visibleMemory(record, context)) {
            result.records.append(record);
        }
    }
    if (q.lastError().isValid()) {
        return {};
    }
    return result;
}

bool SQLiteMemoryStore::hasScopedMemory() const {
    if (QThread::currentThread() != connectionThread_ || !isAvailable()) {
        return true;
    }
    QSqlQuery query(database_);
    if (!query.exec("SELECT 1 FROM scoped_memory_entries LIMIT 1")) {
        return true;
    }
    return query.next();
}

void SQLiteMemoryStore::clear() {
    if (!isAvailable()) {
        setLastError(QStringLiteral("SQLite memory database is not open."));
        return;
    }

    if (!database_.transaction()) {
        setLastError(QStringLiteral("SQLite memory database failed to begin transaction."));
        return;
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("DELETE FROM memory_entries")) ||
        !query.exec(QStringLiteral("DELETE FROM scoped_memory_entries"))) {
        database_.rollback();
        setLastError(query.lastError().text());
    } else if (!database_.commit()) {
        setLastError(QStringLiteral("SQLite memory database failed to commit clear transaction."));
    } else {
        setLastError({});
    }
}

bool SQLiteMemoryStore::isAvailable() const {
    // Cross-thread readers use their own read-only connection and immutable initialization state.
    if (QThread::currentThread() != connectionThread_) {
        return identityReady_;
    }
    return database_.isOpen() && identityReady_;
}

QString SQLiteMemoryStore::lastError() const {
    return lastError_;
}

QString SQLiteMemoryStore::databasePath() const {
    return databasePath_;
}

int SQLiteMemoryStore::schemaVersion() const {
    if (!database_.isOpen()) {
        return 0;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT value FROM memory_schema_metadata WHERE key = ?"));
    query.addBindValue(QStringLiteral("schema_version"));

    if (!query.exec() || !query.next()) {
        return 0;
    }

    return query.value(0).toInt();
}

void SQLiteMemoryStore::open() {
    const QFileInfo fileInfo(databasePath_);
    if (!fileInfo.dir().exists()) {
        QDir().mkpath(fileInfo.dir().absolutePath());
    }

    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(databasePath_);
    if (!database_.open()) {
        setLastError(database_.lastError().text());
    } else {
        applySqlitePerformancePragmas(database_);
        QFile::setPermissions(databasePath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        setLastError({});
    }
}

void SQLiteMemoryStore::initializeSchema() {
    if (!database_.isOpen()) {
        setLastError(QStringLiteral("SQLite memory database is not open."));
        return;
    }

    QSqlQuery versionCheck(database_);
    if (!versionCheck.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' "
                                          "AND name='memory_schema_metadata'"))) {
        setLastError(versionCheck.lastError().text());
        database_.close();
        return;
    }
    const bool hasMetadata = versionCheck.next();
    versionCheck.finish();
    if (hasMetadata) {
        if (!versionCheck.exec(QStringLiteral("SELECT value FROM memory_schema_metadata "
                                               "WHERE key='schema_version'"))) {
            setLastError(versionCheck.lastError().text());
            database_.close();
            return;
        }
        if (versionCheck.next() && versionCheck.value(0).toInt() > currentSchemaVersion) {
            setLastError(QStringLiteral("Unsupported memory schema version."));
            versionCheck.finish();
            database_.close();
            return;
        }
        versionCheck.finish();
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS memory_entries("
                                   "id INTEGER PRIMARY KEY,"
                                   "key TEXT UNIQUE NOT NULL,"
                                   "value TEXT NOT NULL)"))) {
        setLastError(query.lastError().text());
        return;
    }

    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS memory_schema_metadata("
                                   "key TEXT PRIMARY KEY NOT NULL,"
                                   "value INTEGER NOT NULL)"))) {
        setLastError(query.lastError().text());
        return;
    }

    QSqlQuery columns(database_);
    if (!columns.exec(QStringLiteral("PRAGMA table_info(memory_entries)"))) {
        setLastError(columns.lastError().text());
        return;
    }
    bool hasStableId = false;
    while (columns.next())
        if (columns.value(1).toString() == QLatin1String("id") &&
            columns.value(5).toInt() == 1)
            hasStableId = true;
    columns.finish();
    if (!hasStableId) {
        if (!database_.transaction()) {
            setLastError(database_.lastError().text());
            return;
        }
        const QStringList migration{
            QStringLiteral("DROP TRIGGER IF EXISTS memory_fts_insert"),
            QStringLiteral("DROP TRIGGER IF EXISTS memory_fts_delete"),
            QStringLiteral("DROP TRIGGER IF EXISTS memory_fts_update"),
            QStringLiteral("CREATE TABLE memory_entries_new(id INTEGER PRIMARY KEY, "
                           "key TEXT UNIQUE NOT NULL, value TEXT NOT NULL)"),
            QStringLiteral("INSERT INTO memory_entries_new(key, value) "
                           "SELECT key, value FROM memory_entries ORDER BY key COLLATE BINARY"),
            QStringLiteral("DROP TABLE memory_entries"),
            QStringLiteral("ALTER TABLE memory_entries_new RENAME TO memory_entries")};
        for (const auto& statement : migration) {
            if (!query.exec(statement)) {
                const QString error = query.lastError().text();
                database_.rollback();
                setLastError(error);
                return;
            }
        }
        if (!database_.commit()) {
            const QString error = database_.lastError().text();
            database_.rollback();
            setLastError(error);
            return;
        }
    }
    identityReady_ = true;

    const int previousVersion = schemaVersion();
    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name=?"));
    existing.addBindValue(QStringLiteral("memory_entries_fts"));
    const bool hadIndex = existing.exec() && existing.next();
    QSqlQuery triggerCheck(database_);
    triggerCheck.exec(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type='trigger' "
                                           "AND name IN ('memory_fts_insert','memory_fts_delete','memory_fts_update')"));
    const bool hadTriggers = triggerCheck.next() && triggerCheck.value(0).toInt() == 3;
    triggerCheck.finish();
    existing.finish();
    if (database_.transaction()) {
        const QStringList statements{
            QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS memory_entries_fts USING fts5("
                           "memory_id UNINDEXED, key, value, tokenize='unicode61')"),
            QStringLiteral("CREATE TRIGGER IF NOT EXISTS memory_fts_insert AFTER INSERT ON memory_entries "
                           "BEGIN INSERT INTO memory_entries_fts(memory_id, key, value) "
                           "VALUES(new.id, new.key, new.value); END"),
            QStringLiteral("CREATE TRIGGER IF NOT EXISTS memory_fts_delete AFTER DELETE ON memory_entries "
                           "BEGIN DELETE FROM memory_entries_fts WHERE memory_id=old.id; END"),
            QStringLiteral("CREATE TRIGGER IF NOT EXISTS memory_fts_update AFTER UPDATE ON memory_entries "
                           "BEGIN DELETE FROM memory_entries_fts WHERE memory_id=old.id; "
                           "INSERT INTO memory_entries_fts(memory_id, key, value) "
                           "VALUES(new.id, new.key, new.value); END")};
        bool ready = true;
        if (hadIndex && previousVersion < 3) {
            for (const auto& name : {QStringLiteral("memory_fts_insert"),
                                     QStringLiteral("memory_fts_delete"),
                                     QStringLiteral("memory_fts_update")})
                if (!query.exec(QStringLiteral("DROP TRIGGER IF EXISTS %1").arg(name))) {
                    ready = false;
                    break;
                }
            if (ready)
                ready = query.exec(QStringLiteral("DROP TABLE memory_entries_fts"));
        }
        for (const auto& statement : statements)
            if (ready && !query.exec(statement)) {
                ready = false;
                break;
            }
        if (ready && (previousVersion < 3 || !hadIndex || !hadTriggers)) {
            ready = query.exec(QStringLiteral("DELETE FROM memory_entries_fts"));
            if (ready)
                ready = query.exec(QStringLiteral("INSERT INTO memory_entries_fts(memory_id, key, value) "
                                                   "SELECT id, key, value FROM memory_entries"));
        }
        if (ready && database_.commit())
            ftsReady_ = true;
        else
            database_.rollback();
    }

    if (!database_.transaction()) {
        identityReady_ = false;
        return;
    }
    const auto rollbackScoped = qScopeGuard([&] {
        if (!identityReady_) {
            database_.rollback();
        }
    });
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS scoped_memory_entries("
            "id TEXT PRIMARY KEY NOT NULL,scope INTEGER NOT NULL CHECK(scope IN (1,2,3)),"
            "owner_id TEXT NOT NULL,workspace_id TEXT NOT NULL,key TEXT NOT NULL,value TEXT NOT "
            "NULL,"
            "provenance INTEGER NOT NULL CHECK(provenance BETWEEN 0 AND 6),source_ref TEXT NOT "
            "NULL,"
            "created_at TEXT NOT NULL,updated_at TEXT NOT NULL,"
            "UNIQUE(scope,owner_id,workspace_id,key),"
            "CHECK((scope=1 AND owner_id<>'') OR (scope=2 AND owner_id<>'' AND "
            "owner_id=workspace_id) OR "
            "(scope=3 AND owner_id='' AND workspace_id='')))")) ||
        !query.exec(
            QStringLiteral("CREATE INDEX IF NOT EXISTS scoped_memory_owner ON "
                           "scoped_memory_entries(scope,owner_id,workspace_id,updated_at)"))) {
        identityReady_ = false;
        setLastError(QStringLiteral("Scoped memory migration failed."));
        return;
    }
    query.prepare(QStringLiteral("INSERT INTO memory_schema_metadata(key, value) VALUES(?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(QStringLiteral("schema_version"));
    query.addBindValue(currentSchemaVersion);
    if (!query.exec() || !database_.commit()) {
        identityReady_ = false;
        setLastError(QStringLiteral("Scoped memory migration failed."));
        return;
    }

    setLastError({});
}

void SQLiteMemoryStore::setLastError(QString error) const {
    lastError_ = std::move(error);
}

} // namespace sentinel::core
