// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/chat/SQLiteChatHistoryStore.h"

#include "sentinel/core/memory/SqlitePragmas.h"
#include "sentinel/core/memory/FtsQuery.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <algorithm>

namespace sentinel::core {

namespace {

ChatRole roleFromName(const QString& role) {
    if (role == QStringLiteral("user")) {
        return ChatRole::User;
    }
    if (role == QStringLiteral("assistant")) {
        return ChatRole::Assistant;
    }
    return ChatRole::System;
}

ChatMessageStatus statusFromName(const QString& status) {
    if (status == QStringLiteral("sent")) {
        return ChatMessageStatus::Sent;
    }
    if (status == QStringLiteral("error")) {
        return ChatMessageStatus::Error;
    }
    if (status == QStringLiteral("interrupted")) return ChatMessageStatus::Interrupted;
    if (status == QStringLiteral("sending")) return ChatMessageStatus::Sending;
    if (status == QStringLiteral("streaming")) return ChatMessageStatus::Streaming;
    return ChatMessageStatus::Received;
}

ChatMessage messageFromQuery(const QSqlQuery& query) {
    return ChatMessage{
        query.value(0).toInt(), roleFromName(query.value(1).toString()),
        query.value(2).toString(),
        QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs),
        statusFromName(query.value(4).toString()),
        query.value(5).toString(), query.value(6).toString(), query.value(7).toString(),
        query.value(8).isNull() ? -1 : query.value(8).toLongLong(),
        query.value(9).isNull() ? -1 : query.value(9).toLongLong(),
        query.value(10).isNull() ? 0.0 : query.value(10).toDouble()};
}

const QString kMessageColumns = QStringLiteral(
    "id, role, content, timestamp, status, provider_used, model_used, role_used, "
    "response_duration_ms, first_token_latency_ms, approx_tokens_per_second");
const QString kJoinedMessageColumns = QStringLiteral(
    "m.id, m.role, m.content, m.timestamp, m.status, m.provider_used, m.model_used, "
    "m.role_used, m.response_duration_ms, m.first_token_latency_ms, "
    "m.approx_tokens_per_second");

static const QStringList kKnownTables = {
    QStringLiteral("chat_messages"),
};

bool tableHasColumn(const QSqlDatabase& database, const QString& table, const QString& column) {
    if (!kKnownTables.contains(table)) {
        return false;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        return false;
    }
    while (query.next()) {
        if (query.value(1).toString() == column) {
            return true;
        }
    }
    return false;
}

} // namespace

SQLiteChatHistoryStore::SQLiteChatHistoryStore(QString databasePath,
                                               std::shared_ptr<StorageAdmission> admission,
                                               std::uint64_t generation)
    : admission_(std::move(admission)), generation_(generation   ? generation
                                                    : admission_ ? admission_->snapshot().generation
                                                                 : 0),
      databasePath_(std::move(databasePath)),
      connectionName_(
          QStringLiteral("sentinel_chat_%1").arg(QUuid::createUuid().toString(QUuid::Id128))) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return;
    }
    open();
    initializeSchema();
}

StorageAdmission::Writer SQLiteChatHistoryStore::admitWrite() {
    if (!admission_) {
        return {};
    }
    auto writer = admission_->admit(generation_);
    if (!writer) {
        lastError_ = QStringLiteral("Storage admission refused (busy, stale or shutdown).");
    }
    return writer;
}

SQLiteChatHistoryStore::~SQLiteChatHistoryStore() {
    const auto connectionName = connectionName_;
    database_ = {};
    QSqlDatabase::removeDatabase(connectionName);
}

QList<ChatMessage> SQLiteChatHistoryStore::loadMessages() const {
    QList<ChatMessage> result;
    if (!database_.isOpen()) {
        setLastError(QStringLiteral("SQLite chat history database is not open."));
        return result;
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT id, role, content, timestamp, status, "
                                   "provider_used, model_used, role_used, "
                                   "response_duration_ms, first_token_latency_ms, "
                                   "approx_tokens_per_second "
                                   "FROM chat_messages ORDER BY id ASC"))) {
        setLastError(query.lastError().text());
        return result;
    }

    setLastError({});
    while (query.next()) {
        result.append(messageFromQuery(query));
    }

    return result;
}

QList<ChatMessage> SQLiteChatHistoryStore::recentMessages(int limit) const {
    QList<ChatMessage> result;
    if (limit <= 0 || !database_.isOpen())
        return result;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM chat_messages WHERE role IN ('user','assistant') "
                                 "AND status != 'error' ORDER BY id DESC LIMIT ?").arg(kMessageColumns));
    query.addBindValue(qBound(1, limit, 100));
    if (!query.exec()) {
        setLastError(query.lastError().text());
        return result;
    }
    setLastError({});
    while (query.next())
        result.prepend(messageFromQuery(query));
    return result;
}

QList<ChatMessage> SQLiteChatHistoryStore::searchMessages(const QString& text, int limit,
                                                           int beforeId) const {
    QList<ChatMessage> result;
    if (text.trimmed().isEmpty() || limit <= 0 || !database_.isOpen())
        return result;
    const QString match = ftsMatchQuery(text);
    if (ftsReady_ && !match.isEmpty()) {
        QSqlQuery indexed(database_);
        indexed.prepare(QStringLiteral(
            "SELECT %1 FROM chat_messages_fts "
            "JOIN chat_messages AS m ON m.id = chat_messages_fts.rowid "
            "WHERE chat_messages_fts MATCH ? AND m.role IN ('user','assistant') "
            "AND m.status != 'error' AND (? = 0 OR m.id < ?) "
            "ORDER BY bm25(chat_messages_fts), m.id DESC LIMIT ?")
            .arg(kJoinedMessageColumns));
        indexed.addBindValue(match);
        indexed.addBindValue(beforeId);
        indexed.addBindValue(beforeId);
        indexed.addBindValue(qBound(1, limit, 50));
        if (indexed.exec()) {
            setLastError({});
            while (indexed.next())
                result.append(messageFromQuery(indexed));
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
        conditions.append(QStringLiteral("content LIKE ? ESCAPE '\\'"));
        scores.append(QStringLiteral("CASE WHEN content LIKE ? ESCAPE '\\' THEN 1 ELSE 0 END"));
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT %1 FROM chat_messages WHERE role IN ('user','assistant') "
                                 "AND status != 'error' AND (? = 0 OR id < ?) AND (%2) "
                                 "ORDER BY (%3) DESC, id DESC LIMIT ?")
                      .arg(kMessageColumns, conditions.join(QStringLiteral(" OR ")),
                           scores.join(QLatin1Char('+'))));
    auto pattern = [](QString word) {
        word.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        word.replace(QLatin1Char('%'), QStringLiteral("\\%"));
        word.replace(QLatin1Char('_'), QStringLiteral("\\_"));
        return QLatin1Char('%') + word + QLatin1Char('%');
    };
    query.addBindValue(beforeId);
    query.addBindValue(beforeId);
    for (const auto& word : words)
        query.addBindValue(pattern(word));
    for (const auto& word : words)
        query.addBindValue(pattern(word));
    query.addBindValue(qBound(1, limit, 50));
    if (!query.exec()) {
        setLastError(query.lastError().text());
        return result;
    }
    setLastError({});
    while (query.next())
        result.append(messageFromQuery(query));
    return result;
}

void SQLiteChatHistoryStore::appendMessage(const ChatMessage& message) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return;
    }
    if (!database_.isOpen()) {
        setLastError(QStringLiteral("SQLite chat history database is not open."));
        return;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO chat_messages("
                                 "id, role, content, timestamp, status, provider_used, "
                                 "model_used, role_used, response_duration_ms, "
                                 "first_token_latency_ms, approx_tokens_per_second) "
                                 "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                                 "ON CONFLICT(id) DO UPDATE SET "
                                 "role = excluded.role,"
                                 "content = excluded.content,"
                                 "timestamp = excluded.timestamp,"
                                 "status = excluded.status,"
                                 "provider_used = excluded.provider_used,"
                                 "model_used = excluded.model_used,"
                                 "role_used = excluded.role_used,"
                                 "response_duration_ms = excluded.response_duration_ms,"
                                 "first_token_latency_ms = excluded.first_token_latency_ms,"
                                 "approx_tokens_per_second = excluded.approx_tokens_per_second"));
    query.addBindValue(message.id);
    query.addBindValue(chatRoleName(message.role));
    query.addBindValue(message.content);
    query.addBindValue(message.timestamp.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(chatMessageStatusName(message.status));
    query.addBindValue(message.providerUsed);
    query.addBindValue(message.modelUsed);
    query.addBindValue(message.roleUsed);
    query.addBindValue(message.responseDurationMs >= 0 ? QVariant(message.responseDurationMs)
                                                       : QVariant{});
    query.addBindValue(message.firstTokenLatencyMs >= 0 ? QVariant(message.firstTokenLatencyMs)
                                                        : QVariant{});
    query.addBindValue(message.approximateTokensPerSecond > 0.0
                           ? QVariant(message.approximateTokensPerSecond)
                           : QVariant{});
    if (!query.exec()) {
        setLastError(query.lastError().text());
    } else {
        setLastError({});
    }
}

void SQLiteChatHistoryStore::clear() {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return;
    }
    if (!database_.isOpen()) {
        setLastError(QStringLiteral("SQLite chat history database is not open."));
        return;
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("DELETE FROM chat_messages"))) {
        setLastError(query.lastError().text());
    } else {
        setLastError({});
    }
}

int SQLiteChatHistoryStore::pruneCompletedBefore(const QDateTime& cutoffUtc, int limit) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return -1;
    }
    if (!database_.isOpen() || !cutoffUtc.isValid() || limit < 1 || limit > 500 ||
        !database_.transaction()) {
        setLastError(QStringLiteral("Chat retention unavailable."));
        return -1;
    }
    QSqlQuery active(database_);
    if (!active.exec(QStringLiteral("SELECT 1 FROM chat_messages WHERE status IN "
                                    "('sending','streaming','queued') LIMIT 1"))) {
        database_.rollback();
        setLastError(active.lastError().text());
        return -1;
    }
    if (active.next()) {
        active.finish();
        if (database_.commit()) return 0;
        setLastError(database_.lastError().text());
        return -1;
    }
    active.finish();
    QSqlQuery select(database_);
    select.prepare(QStringLiteral("SELECT id FROM chat_messages WHERE timestamp<? "
                                  "AND status NOT IN ('sending','streaming','queued') "
                                  "ORDER BY timestamp LIMIT ?"));
    select.addBindValue(cutoffUtc.toUTC().toString(Qt::ISODateWithMs));
    select.addBindValue(limit);
    if (!select.exec()) {
        database_.rollback();
        setLastError(select.lastError().text());
        return -1;
    }
    QList<int> ids;
    while (select.next()) ids.append(select.value(0).toInt());
    select.finish();
    QSqlQuery remove(database_);
    remove.prepare(QStringLiteral("DELETE FROM chat_messages WHERE id=?"));
    for (const auto id : ids) {
        remove.bindValue(0, id);
        if (!remove.exec()) {
            database_.rollback();
            setLastError(remove.lastError().text());
            return -1;
        }
    }
    if (!database_.commit()) {
        database_.rollback();
        setLastError(database_.lastError().text());
        return -1;
    }
    setLastError({});
    return ids.size();
}

bool SQLiteChatHistoryStore::isAvailable() const {
    return database_.isOpen();
}

QString SQLiteChatHistoryStore::lastError() const {
    return lastError_;
}

QString SQLiteChatHistoryStore::databasePath() const {
    return databasePath_;
}

int SQLiteChatHistoryStore::schemaVersion() const {
    if (!database_.isOpen()) {
        return 0;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT value FROM chat_history_schema_metadata WHERE key = ?"));
    query.addBindValue(QStringLiteral("schema_version"));

    if (!query.exec() || !query.next()) {
        return 0;
    }

    return query.value(0).toInt();
}

void SQLiteChatHistoryStore::open() {
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

void SQLiteChatHistoryStore::initializeSchema() {
    if (!database_.isOpen()) {
        setLastError(QStringLiteral("SQLite chat history database is not open."));
        return;
    }

    QSqlQuery versionCheck(database_);
    if (!versionCheck.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' "
                                          "AND name='chat_history_schema_metadata'"))) {
        setLastError(versionCheck.lastError().text());
        database_.close();
        return;
    }
    const bool hasMetadata = versionCheck.next();
    versionCheck.finish();
    if (hasMetadata) {
        if (!versionCheck.exec(QStringLiteral("SELECT value FROM chat_history_schema_metadata "
                                               "WHERE key='schema_version'"))) {
            setLastError(versionCheck.lastError().text());
            database_.close();
            return;
        }
        if (versionCheck.next() && versionCheck.value(0).toInt() > currentSchemaVersion) {
            setLastError(QStringLiteral("Unsupported chat history schema version."));
            versionCheck.finish();
            database_.close();
            return;
        }
        versionCheck.finish();
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS chat_messages("
                                   "id INTEGER PRIMARY KEY NOT NULL,"
                                   "role TEXT NOT NULL,"
                                   "content TEXT NOT NULL,"
                                   "timestamp TEXT NOT NULL,"
                                   "status TEXT NOT NULL,"
                                   "provider_used TEXT,"
                                   "model_used TEXT,"
                                   "role_used TEXT,"
                                   "response_duration_ms INTEGER,"
                                   "first_token_latency_ms INTEGER,"
                                   "approx_tokens_per_second REAL)"))) {
        setLastError(query.lastError().text());
        return;
    }

    const QList<QPair<QString, QString>> columns{
        {QStringLiteral("provider_used"), QStringLiteral("TEXT")},
        {QStringLiteral("model_used"), QStringLiteral("TEXT")},
        {QStringLiteral("role_used"), QStringLiteral("TEXT")},
        {QStringLiteral("response_duration_ms"), QStringLiteral("INTEGER")},
        {QStringLiteral("first_token_latency_ms"), QStringLiteral("INTEGER")},
        {QStringLiteral("approx_tokens_per_second"), QStringLiteral("REAL")},
    };
    const QStringList knownColumnNames = {
        QStringLiteral("provider_used"),
        QStringLiteral("model_used"),
        QStringLiteral("role_used"),
        QStringLiteral("response_duration_ms"),
        QStringLiteral("first_token_latency_ms"),
        QStringLiteral("approx_tokens_per_second"),
    };
    for (const auto& column : columns) {
        if (!knownColumnNames.contains(column.first)) {
            continue;
        }
        if (!tableHasColumn(database_, QStringLiteral("chat_messages"), column.first) &&
            !query.exec(QStringLiteral("ALTER TABLE chat_messages ADD COLUMN %1 %2")
                            .arg(column.first, column.second))) {
            setLastError(query.lastError().text());
            return;
        }
    }

    if (!query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS chat_messages_recent_idx "
                                   "ON chat_messages(id DESC) WHERE role IN ('user','assistant') "
                                   "AND status != 'error'"))) {
        setLastError(query.lastError().text());
        return;
    }

    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS chat_history_schema_metadata("
                                   "key TEXT PRIMARY KEY NOT NULL,"
                                   "value INTEGER NOT NULL)"))) {
        setLastError(query.lastError().text());
        return;
    }

    const int previousVersion = schemaVersion();
    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' AND name=?"));
    existing.addBindValue(QStringLiteral("chat_messages_fts"));
    const bool hadIndex = existing.exec() && existing.next();
    QSqlQuery triggerCheck(database_);
    triggerCheck.exec(QStringLiteral("SELECT count(*) FROM sqlite_master WHERE type='trigger' "
                                           "AND name IN ('chat_fts_insert','chat_fts_delete','chat_fts_update')"));
    const bool hadTriggers = triggerCheck.next() && triggerCheck.value(0).toInt() == 3;
    triggerCheck.finish();
    existing.finish();
    if (database_.transaction()) {
        const QStringList statements{
            QStringLiteral("CREATE VIRTUAL TABLE IF NOT EXISTS chat_messages_fts USING fts5("
                           "content, tokenize='unicode61')"),
            QStringLiteral("CREATE TRIGGER IF NOT EXISTS chat_fts_insert AFTER INSERT ON chat_messages "
                           "WHEN new.role IN ('user','assistant') AND new.status != 'error' "
                           "BEGIN INSERT INTO chat_messages_fts(rowid, content) "
                           "VALUES(new.id, new.content); END"),
            QStringLiteral("CREATE TRIGGER IF NOT EXISTS chat_fts_delete AFTER DELETE ON chat_messages "
                           "WHEN old.role IN ('user','assistant') AND old.status != 'error' "
                           "BEGIN DELETE FROM chat_messages_fts WHERE rowid=old.id; END"),
            QStringLiteral("CREATE TRIGGER IF NOT EXISTS chat_fts_update AFTER UPDATE ON chat_messages "
                           "BEGIN DELETE FROM chat_messages_fts WHERE rowid=old.id; "
                           "INSERT INTO chat_messages_fts(rowid, content) "
                           "SELECT new.id, new.content WHERE new.role IN ('user','assistant') "
                           "AND new.status != 'error'; END")};
        bool ready = true;
        for (const auto& statement : statements)
            if (!query.exec(statement)) {
                ready = false;
                break;
            }
        if (ready && (previousVersion < currentSchemaVersion || !hadIndex || !hadTriggers)) {
            ready = query.exec(QStringLiteral("DELETE FROM chat_messages_fts"));
            if (ready)
                ready = query.exec(QStringLiteral("INSERT INTO chat_messages_fts(rowid, content) "
                                                   "SELECT id, content FROM chat_messages "
                                                   "WHERE role IN ('user','assistant') "
                                                   "AND status != 'error'"));
        }
        if (ready && database_.commit())
            ftsReady_ = true;
        else
            database_.rollback();
    }

    query.prepare(QStringLiteral("INSERT INTO chat_history_schema_metadata(key, value) "
                                 "VALUES(?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(QStringLiteral("schema_version"));
    query.addBindValue(ftsReady_ ? currentSchemaVersion : qMax(2, previousVersion));
    if (!query.exec()) {
        setLastError(query.lastError().text());
        return;
    }

    if (!query.exec(QStringLiteral("UPDATE chat_messages SET status='interrupted' "
                                   "WHERE status IN ('sending','streaming')"))) {
        setLastError(query.lastError().text());
        return;
    }
    setLastError({});
}

void SQLiteChatHistoryStore::setLastError(QString error) const {
    lastError_ = std::move(error);
}

} // namespace sentinel::core
