// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/chat/SQLiteConversationStore.h"

#include "sentinel/core/memory/SqlitePragmas.h"

#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>
#include <QVariant>

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
    if (status == QStringLiteral("queued")) return ChatMessageStatus::Queued;
    if (status == QStringLiteral("sending")) return ChatMessageStatus::Sending;
    if (status == QStringLiteral("streaming")) return ChatMessageStatus::Streaming;
    if (status == QStringLiteral("completed")) return ChatMessageStatus::Completed;
    if (status == QStringLiteral("failed")) return ChatMessageStatus::Failed;
    if (status == QStringLiteral("cancelled")) return ChatMessageStatus::Cancelled;
    if (status == QStringLiteral("interrupted")) return ChatMessageStatus::Interrupted;
    return ChatMessageStatus::Received;
}

ChatProviderErrorCategory categoryFromName(const QString& name) {
    for (const auto category : {ChatProviderErrorCategory::None,
             ChatProviderErrorCategory::AuthenticationRequired,
             ChatProviderErrorCategory::ModelNotFound,
             ChatProviderErrorCategory::ProviderUnavailable,
             ChatProviderErrorCategory::ConnectionFailed,
             ChatProviderErrorCategory::Timeout,
             ChatProviderErrorCategory::RateLimited,
             ChatProviderErrorCategory::RequestRejected,
             ChatProviderErrorCategory::CapabilityUnsupported,
             ChatProviderErrorCategory::MalformedResponse,
             ChatProviderErrorCategory::Cancelled,
             ChatProviderErrorCategory::ProviderFailure})
        if (chatProviderErrorCategoryName(category) == name) return category;
    return ChatProviderErrorCategory::None;
}

QString boolText(bool value) {
    return value ? QStringLiteral("1") : QStringLiteral("0");
}

static const QStringList kKnownTables = {
    QStringLiteral("conversations"),
    QStringLiteral("conversation_messages"),
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

SQLiteConversationStore::SQLiteConversationStore(QString databasePath,
                                                 std::shared_ptr<StorageAdmission> admission,
                                                 std::uint64_t generation)
    : admission_(std::move(admission)), generation_(generation   ? generation
                                                    : admission_ ? admission_->snapshot().generation
                                                                 : 0),
      databasePath_(std::move(databasePath)),
      connectionName_(QStringLiteral("sentinel_conversations_%1")
                          .arg(QUuid::createUuid().toString(QUuid::Id128))) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return;
    }
    open();
    initializeSchema();
}

SQLiteConversationStore::~SQLiteConversationStore() {
    const auto connectionName = connectionName_;
    database_ = {};
    QSqlDatabase::removeDatabase(connectionName);
}

StorageAdmission::Writer SQLiteConversationStore::admitWrite() {
    if (!admission_) {
        return {};
    }
    auto writer = admission_->admit(generation_);
    if (!writer) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("Storage admission refused (busy, stale or shutdown)."));
    }
    return writer;
}

QString SQLiteConversationStore::normalizedTitle(const QString& title) {
    const auto trimmed = title.trimmed();
    return trimmed.isEmpty() ? QStringLiteral("Untitled Conversation") : trimmed;
}

ConversationRecord SQLiteConversationStore::createConversation(const QString& title) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return {};
    }
    ConversationRecord record;
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return record;
    }

    record.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record.title = normalizedTitle(title);
    record.createdAtUtc = QDateTime::currentDateTimeUtc();
    record.updatedAtUtc = record.createdAtUtc;
    record.summary = conversationRecordSummary(record);

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO conversations("
                                 "id, title, created_at, updated_at, archived, pinned, deleted) "
                                 "VALUES(?, ?, ?, ?, 0, 0, 0)"));
    query.addBindValue(record.id);
    query.addBindValue(record.title);
    query.addBindValue(record.createdAtUtc.toString(Qt::ISODateWithMs));
    query.addBindValue(record.updatedAtUtc.toString(Qt::ISODateWithMs));
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return {};
    }

    setLastError(ConversationStoreErrorCode::None, {});
    return record;
}

QList<ConversationRecord> SQLiteConversationStore::listConversations() const {
    QList<ConversationRecord> records;
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return records;
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT c.id, c.title, c.created_at, c.updated_at, "
                                   "c.archived, c.pinned, c.deleted, c.user_renamed, COUNT(m.message_id) "
                                   "FROM conversations c "
                                   "LEFT JOIN conversation_messages m ON m.conversation_id = c.id "
                                   "WHERE c.deleted = 0 "
                                   "GROUP BY c.id "
                                   "ORDER BY c.pinned DESC, c.updated_at DESC, c.rowid DESC"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return records;
    }

    while (query.next()) {
        ConversationRecord record;
        record.id = query.value(0).toString();
        record.title = query.value(1).toString();
        record.createdAtUtc = QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs);
        record.updatedAtUtc = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs);
        record.archived = query.value(4).toBool();
        record.pinned = query.value(5).toBool();
        record.deleted = query.value(6).toBool();
        record.userRenamed = query.value(7).toBool();
        record.messageCount = query.value(8).toInt();
        record.summary = conversationRecordSummary(record);
        records.append(record);
    }

    setLastError(ConversationStoreErrorCode::None, {});
    return records;
}

QList<ConversationRecord> SQLiteConversationStore::searchConversations(
    const QString& queryText, int limit) const {
    QList<ConversationRecord> records;
    const auto needle = queryText.trimmed().left(128);
    if (!database_.isOpen() || needle.isEmpty()) return records;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT c.id, c.title, c.created_at, c.updated_at, c.archived, c.pinned, "
        "c.deleted, c.user_renamed, "
        "(SELECT COUNT(*) FROM conversation_messages m WHERE m.conversation_id = c.id) "
        "FROM conversations c WHERE c.deleted = 0 AND "
        "(instr(lower(c.title), lower(?)) > 0 OR EXISTS "
        "(SELECT 1 FROM conversation_messages m WHERE m.conversation_id = c.id "
        "AND instr(lower(m.content), lower(?)) > 0)) "
        "ORDER BY c.pinned DESC, c.updated_at DESC, c.id ASC LIMIT ?"));
    query.addBindValue(needle);
    query.addBindValue(needle);
    query.addBindValue(qBound(1, limit, 50));
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return records;
    }
    while (query.next()) {
        ConversationRecord record;
        record.id = query.value(0).toString();
        record.title = query.value(1).toString();
        record.createdAtUtc = QDateTime::fromString(query.value(2).toString(), Qt::ISODateWithMs);
        record.updatedAtUtc = QDateTime::fromString(query.value(3).toString(), Qt::ISODateWithMs);
        record.archived = query.value(4).toBool();
        record.pinned = query.value(5).toBool();
        record.deleted = query.value(6).toBool();
        record.userRenamed = query.value(7).toBool();
        record.messageCount = query.value(8).toInt();
        record.summary = conversationRecordSummary(record);
        records.append(record);
    }
    setLastError(ConversationStoreErrorCode::None, {});
    return records;
}

bool SQLiteConversationStore::appendMessage(const ConversationMessageRecord& message) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return false;
    }
    if (message.conversationId.trimmed().isEmpty() || !conversationExists(message.conversationId)) {
        setLastError(ConversationStoreErrorCode::InvalidConversationId,
                     QStringLiteral("Conversation does not exist."));
        return false;
    }
    if (conversationArchived(message.conversationId)) {
        setLastError(ConversationStoreErrorCode::UnsupportedOperation,
                     QStringLiteral("Archived conversation cannot accept new messages."));
        return false;
    }

    if (!database_.transaction()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, database_.lastError().text());
        return false;
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO conversation_messages("
                                 "conversation_id, message_id, role, content, timestamp, status, "
                                 "provider_id, model_id, reply_to_id, replaces_id, partial, error_category, attachments_json) "
                                 "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
                                 "ON CONFLICT(conversation_id, message_id) DO UPDATE SET "
                                 "role = excluded.role,"
                                 "content = excluded.content,"
                                 "timestamp = excluded.timestamp,"
                                 "status = excluded.status,"
                                 "provider_id = excluded.provider_id,"
                                 "model_id = excluded.model_id,"
                                 "reply_to_id = excluded.reply_to_id,"
                                 "replaces_id = excluded.replaces_id,"
                                 "partial = excluded.partial,"
                                 "error_category = excluded.error_category, attachments_json = excluded.attachments_json"));
    query.addBindValue(message.conversationId);
    query.addBindValue(message.messageId);
    query.addBindValue(chatRoleName(message.role));
    query.addBindValue(message.content);
    query.addBindValue(message.timestampUtc.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(chatMessageStatusName(message.status));
    query.addBindValue(message.providerId.isNull() ? QStringLiteral("") : message.providerId);
    query.addBindValue(message.modelId.isNull() ? QStringLiteral("") : message.modelId);
    query.addBindValue(message.replyToMessageId);
    query.addBindValue(message.replacesMessageId);
    query.addBindValue(message.partial ? 1 : 0);
    query.addBindValue(chatProviderErrorCategoryName(message.errorCategory));
    query.addBindValue(message.attachmentsJson.isNull() ? QStringLiteral("[]") : message.attachmentsJson);
    if (!query.exec()) {
        database_.rollback();
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return false;
    }

    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE conversations SET updated_at = ? WHERE id = ?"));
    update.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    update.addBindValue(message.conversationId);
    if (!update.exec()) {
        database_.rollback();
        setLastError(ConversationStoreErrorCode::StorageFailure, update.lastError().text());
        return false;
    }

    if (!database_.commit()) {
        database_.rollback();
        setLastError(ConversationStoreErrorCode::StorageFailure, database_.lastError().text());
        return false;
    }
    setLastError(ConversationStoreErrorCode::None, {});
    return true;
}

QList<ConversationMessageRecord>
SQLiteConversationStore::loadMessages(const QString& conversationId) const {
    QList<ConversationMessageRecord> records;
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return records;
    }
    if (conversationId.trimmed().isEmpty() || !conversationExists(conversationId)) {
        setLastError(ConversationStoreErrorCode::InvalidConversationId,
                     QStringLiteral("Conversation does not exist."));
        return records;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT conversation_id, message_id, role, content, timestamp, "
                                 "status, provider_id, model_id, reply_to_id, replaces_id, "
                                 "partial, error_category, attachments_json FROM conversation_messages "
                                 "WHERE conversation_id = ? "
                                 "ORDER BY message_id ASC"));
    query.addBindValue(conversationId);
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return records;
    }

    while (query.next()) {
        records.append(ConversationMessageRecord{
            query.value(0).toString(),
            query.value(1).toInt(),
            roleFromName(query.value(2).toString()),
            query.value(3).toString(),
            QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs),
            statusFromName(query.value(5).toString()),
            query.value(6).toString(), query.value(7).toString(),
            query.value(8).toInt(), query.value(9).toInt(),
            query.value(10).toBool(), categoryFromName(query.value(11).toString()), query.value(12).toString(),
        });
    }

    setLastError(ConversationStoreErrorCode::None, {});
    return records;
}

bool SQLiteConversationStore::renameConversation(const QString& conversationId,
                                                 const QString& title) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return false;
    }
    if (!conversationExists(conversationId)) {
        setLastError(ConversationStoreErrorCode::InvalidConversationId,
                     QStringLiteral("Conversation does not exist."));
        return false;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE conversations SET title = ?, updated_at = ?, "
                                 "user_renamed = 1 "
                                 "WHERE id = ? AND deleted = 0"));
    query.addBindValue(normalizedTitle(title));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(conversationId);
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return false;
    }

    setLastError(ConversationStoreErrorCode::None, {});
    return true;
}

bool SQLiteConversationStore::autoTitleConversation(const QString& conversationId,
                                                     const QString& title) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen() || title.trimmed().isEmpty()) return false;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE conversations SET title = ?, updated_at = ? "
                                 "WHERE id = ? AND deleted = 0 AND user_renamed = 0 "
                                 "AND title IN ('Current Transcript', 'Untitled Conversation', 'New Chat', 'New chat')"));
    query.addBindValue(normalizedTitle(title));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(conversationId);
    return query.exec() && query.numRowsAffected() == 1;
}

bool SQLiteConversationStore::updateAutoTitleConversation(const QString& conversationId,
                                                           const QString& title,
                                                           const QString& expectedTitle) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen() || title.trimmed().isEmpty()) return false;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE conversations SET title = ?, updated_at = ? "
                                 "WHERE id = ? AND deleted = 0 AND user_renamed = 0 AND title = ?"));
    query.addBindValue(normalizedTitle(title));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(conversationId);
    query.addBindValue(expectedTitle);
    return query.exec() && query.numRowsAffected() == 1;
}

bool SQLiteConversationStore::archiveConversation(const QString& conversationId) {
    return updateConversationMetadata(conversationId, true, false);
}

bool SQLiteConversationStore::unarchiveConversation(const QString& conversationId) {
    return updateConversationMetadata(conversationId, false, false);
}

bool SQLiteConversationStore::pinConversation(const QString& conversationId) {
    return updatePinnedMetadata(conversationId, true);
}

bool SQLiteConversationStore::unpinConversation(const QString& conversationId) {
    return updatePinnedMetadata(conversationId, false);
}

bool SQLiteConversationStore::clearHistory() {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen() || !database_.transaction()) {
        setLastError(ConversationStoreErrorCode::StorageFailure,
                     QStringLiteral("Conversation store unavailable"));
        return false;
    }
    QSqlQuery query(database_);
    const QStringList statements{
        QStringLiteral("DELETE FROM conversation_summary_metadata"),
        QStringLiteral("DELETE FROM conversation_messages"),
        QStringLiteral("DELETE FROM conversations")};
    for (const auto& statement : statements) {
        if (!query.exec(statement)) {
            setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
            database_.rollback();
            return false;
        }
    }
    if (!database_.commit()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, database_.lastError().text());
        database_.rollback();
        return false;
    }
    setLastError(ConversationStoreErrorCode::None, {});
    return true;
}

bool SQLiteConversationStore::deleteConversation(const QString& conversationId) {
    return updateConversationMetadata(conversationId, true, true);
}

bool SQLiteConversationStore::discardImportedConversation(const QString& conversationId) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen() || !conversationExists(conversationId) || !database_.transaction())
        return false;
    QSqlQuery query(database_);
    for (const auto& table : {QStringLiteral("conversation_summary_metadata"),
                              QStringLiteral("conversation_messages"),
                              QStringLiteral("conversations")}) {
        query.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 = ?").arg(
            table, table == QLatin1String("conversations") ? QStringLiteral("id")
                                                             : QStringLiteral("conversation_id")));
        query.addBindValue(conversationId);
        if (!query.exec()) {
            database_.rollback();
            setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
            return false;
        }
    }
    if (!database_.commit()) {
        setLastError(ConversationStoreErrorCode::StorageFailure,
                     QStringLiteral("Failed to commit imported conversation rollback."));
        return false;
    }
    setLastError(ConversationStoreErrorCode::None, {});
    return true;
}

int SQLiteConversationStore::pruneCompletedBefore(const QDateTime& cutoffUtc, int limit) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return 0;
    }
    if (!database_.isOpen() || !cutoffUtc.isValid() || limit < 1 || limit > 100 ||
        !database_.transaction()) {
        setLastError(ConversationStoreErrorCode::StorageFailure,
                     QStringLiteral("Conversation retention unavailable."));
        return -1;
    }
    QSqlQuery select(database_);
    select.prepare(QStringLiteral("SELECT id FROM conversations c WHERE pinned=0 AND "
        "updated_at<? AND NOT EXISTS (SELECT 1 FROM conversation_messages m "
        "WHERE m.conversation_id=c.id AND m.status IN "
        "('queued','sending','streaming','received')) ORDER BY updated_at LIMIT ?"));
    select.addBindValue(cutoffUtc.toUTC().toString(Qt::ISODateWithMs));
    select.addBindValue(limit);
    if (!select.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, select.lastError().text());
        database_.rollback();
        return -1;
    }
    QStringList ids;
    while (select.next()) ids.append(select.value(0).toString());
    select.finish();
    QSqlQuery remove(database_);
    for (const auto& id : ids) {
        for (const auto& table : {QStringLiteral("conversation_summary_metadata"),
                                  QStringLiteral("conversation_messages"),
                                  QStringLiteral("conversations")}) {
            remove.prepare(QStringLiteral("DELETE FROM %1 WHERE %2=?").arg(table,
                table == QLatin1String("conversations") ? QStringLiteral("id")
                                                       : QStringLiteral("conversation_id")));
            remove.addBindValue(id);
            if (!remove.exec()) {
                setLastError(ConversationStoreErrorCode::StorageFailure, remove.lastError().text());
                database_.rollback();
                return -1;
            }
        }
    }
    if (!database_.commit()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, database_.lastError().text());
        database_.rollback();
        return -1;
    }
    setLastError(ConversationStoreErrorCode::None, {});
    return ids.size();
}

bool SQLiteConversationStore::saveSummaryMetadata(
    const ConversationSummaryMetadataRecord& metadata) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return false;
    }
    if (!conversationExists(metadata.conversationId)) {
        setLastError(ConversationStoreErrorCode::InvalidConversationId,
                     QStringLiteral("Conversation does not exist."));
        return false;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("INSERT INTO conversation_summary_metadata("
                                 "conversation_id, summary_timestamp, covered_first_message_id, "
                                 "covered_last_message_id, estimated_reduction_percent, "
                                 "readiness_state, summary_text, summary) "
                                 "VALUES(?, ?, ?, ?, ?, ?, ?, ?) "
                                 "ON CONFLICT(conversation_id) DO UPDATE SET "
                                 "summary_timestamp = excluded.summary_timestamp,"
                                 "covered_first_message_id = excluded.covered_first_message_id,"
                                 "covered_last_message_id = excluded.covered_last_message_id,"
                                 "estimated_reduction_percent = "
                                 "excluded.estimated_reduction_percent,"
                                 "readiness_state = excluded.readiness_state,"
                                 "summary_text = excluded.summary_text,"
                                 "summary = excluded.summary"));
    query.addBindValue(metadata.conversationId);
    query.addBindValue(metadata.summaryTimestampUtc.toUTC().toString(Qt::ISODateWithMs));
    query.addBindValue(metadata.coveredFirstMessageId);
    query.addBindValue(metadata.coveredLastMessageId);
    query.addBindValue(metadata.estimatedReductionPercent);
    query.addBindValue(metadata.readinessState);
    query.addBindValue(metadata.summaryText);
    query.addBindValue(metadata.summary);
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return false;
    }

    setLastError(ConversationStoreErrorCode::None, {});
    return true;
}

ConversationSummaryMetadataRecord
SQLiteConversationStore::loadSummaryMetadata(const QString& conversationId) const {
    ConversationSummaryMetadataRecord metadata;
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return metadata;
    }
    if (!conversationExists(conversationId)) {
        setLastError(ConversationStoreErrorCode::InvalidConversationId,
                     QStringLiteral("Conversation does not exist."));
        return metadata;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT conversation_id, summary_timestamp, "
                                 "covered_first_message_id, covered_last_message_id, "
                                 "estimated_reduction_percent, readiness_state, summary_text, "
                                 "summary "
                                 "FROM conversation_summary_metadata "
                                 "WHERE conversation_id = ?"));
    query.addBindValue(conversationId);
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return metadata;
    }
    if (!query.next()) {
        setLastError(ConversationStoreErrorCode::None, {});
        return metadata;
    }

    metadata.conversationId = query.value(0).toString();
    metadata.summaryTimestampUtc =
        QDateTime::fromString(query.value(1).toString(), Qt::ISODateWithMs);
    metadata.coveredFirstMessageId = query.value(2).toInt();
    metadata.coveredLastMessageId = query.value(3).toInt();
    metadata.estimatedReductionPercent = query.value(4).toInt();
    metadata.readinessState = query.value(5).toString();
    metadata.summaryText = query.value(6).toString();
    metadata.summary = query.value(7).toString();
    setLastError(ConversationStoreErrorCode::None, {});
    return metadata;
}

ConversationStoreStatus SQLiteConversationStore::status() const {
    return database_.isOpen() ? ConversationStoreStatus::Ready
                              : ConversationStoreStatus::Unavailable;
}

ConversationStoreError SQLiteConversationStore::lastError() const {
    return lastError_;
}

QString SQLiteConversationStore::databasePath() const {
    return databasePath_;
}

int SQLiteConversationStore::schemaVersion() const {
    if (!database_.isOpen()) {
        return 0;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT value FROM conversation_schema_metadata WHERE key = ?"));
    query.addBindValue(QStringLiteral("schema_version"));
    if (!query.exec() || !query.next()) {
        return 0;
    }

    return query.value(0).toInt();
}

bool SQLiteConversationStore::conversationExists(const QString& conversationId) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT 1 FROM conversations WHERE id = ? AND deleted = 0"));
    query.addBindValue(conversationId);
    return query.exec() && query.next();
}

bool SQLiteConversationStore::conversationArchived(const QString& conversationId) const {
    QSqlQuery query(database_);
    query.prepare(
        QStringLiteral("SELECT archived FROM conversations WHERE id = ? AND deleted = 0"));
    query.addBindValue(conversationId);
    return query.exec() && query.next() && query.value(0).toBool();
}

bool SQLiteConversationStore::updateConversationMetadata(const QString& conversationId,
                                                         bool archived, bool deleted) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return false;
    }
    if (!conversationExists(conversationId)) {
        setLastError(ConversationStoreErrorCode::InvalidConversationId,
                     QStringLiteral("Conversation does not exist."));
        return false;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE conversations SET archived = ?, deleted = ?, "
                                 "updated_at = ? WHERE id = ?"));
    query.addBindValue(boolText(archived));
    query.addBindValue(boolText(deleted));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(conversationId);
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return false;
    }

    setLastError(ConversationStoreErrorCode::None, {});
    return true;
}

bool SQLiteConversationStore::updatePinnedMetadata(const QString& conversationId, bool pinned) {
    auto writer = admitWrite();
    if (admission_ && !writer) {
        return false;
    }
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return false;
    }
    if (!conversationExists(conversationId)) {
        setLastError(ConversationStoreErrorCode::InvalidConversationId,
                     QStringLiteral("Conversation does not exist."));
        return false;
    }

    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE conversations SET pinned = ?, updated_at = ? "
                                 "WHERE id = ? AND deleted = 0"));
    query.addBindValue(boolText(pinned));
    query.addBindValue(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    query.addBindValue(conversationId);
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return false;
    }

    setLastError(ConversationStoreErrorCode::None, {});
    return true;
}

void SQLiteConversationStore::open() {
    const QFileInfo fileInfo(databasePath_);
    if (!fileInfo.dir().exists()) {
        QDir().mkpath(fileInfo.dir().absolutePath());
    }

    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(databasePath_);
    if (!database_.open()) {
        setLastError(ConversationStoreErrorCode::Unavailable, database_.lastError().text());
    } else {
        applySqlitePerformancePragmas(database_);
        QFile::setPermissions(databasePath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        setLastError(ConversationStoreErrorCode::None, {});
    }
}

void SQLiteConversationStore::initializeSchema() {
    if (!database_.isOpen()) {
        setLastError(ConversationStoreErrorCode::Unavailable,
                     QStringLiteral("SQLite conversation database is not open."));
        return;
    }

    QSqlQuery versionCheck(database_);
    if (!versionCheck.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='table' "
                                          "AND name='conversation_schema_metadata'"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, versionCheck.lastError().text());
        database_.close();
        return;
    }
    const bool hasMetadata = versionCheck.next();
    versionCheck.finish();
    if (hasMetadata) {
        if (!versionCheck.exec(QStringLiteral("SELECT value FROM conversation_schema_metadata "
                                               "WHERE key='schema_version'"))) {
            setLastError(ConversationStoreErrorCode::StorageFailure, versionCheck.lastError().text());
            database_.close();
            return;
        }
        if (versionCheck.next() && versionCheck.value(0).toInt() > currentSchemaVersion) {
            setLastError(ConversationStoreErrorCode::UnsupportedSchema,
                         QStringLiteral("Conversation schema version is newer than this application."));
            versionCheck.finish();
            database_.close();
            return;
        }
        versionCheck.finish();
    }

    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conversations("
                                   "id TEXT PRIMARY KEY NOT NULL,"
                                   "title TEXT NOT NULL,"
                                   "created_at TEXT NOT NULL,"
                                   "updated_at TEXT NOT NULL,"
                                   "archived INTEGER NOT NULL DEFAULT 0,"
                                   "pinned INTEGER NOT NULL DEFAULT 0,"
                                   "deleted INTEGER NOT NULL DEFAULT 0,"
                                   "user_renamed INTEGER NOT NULL DEFAULT 0)"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }

    if (!tableHasColumn(database_, QStringLiteral("conversations"), QStringLiteral("pinned")) &&
        !query.exec(QStringLiteral(
            "ALTER TABLE conversations ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }
    if (!tableHasColumn(database_, QStringLiteral("conversations"),
                        QStringLiteral("user_renamed")) &&
        !query.exec(QStringLiteral("ALTER TABLE conversations ADD COLUMN user_renamed "
                                   "INTEGER NOT NULL DEFAULT 0"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }

    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conversation_messages("
                                   "conversation_id TEXT NOT NULL,"
                                   "message_id INTEGER NOT NULL,"
                                   "role TEXT NOT NULL,"
                                   "content TEXT NOT NULL,"
                                   "timestamp TEXT NOT NULL,"
                                   "status TEXT NOT NULL,"
                                   "provider_id TEXT NOT NULL DEFAULT '',"
                                   "model_id TEXT NOT NULL DEFAULT '',"
                                   "reply_to_id INTEGER NOT NULL DEFAULT 0,"
                                   "replaces_id INTEGER NOT NULL DEFAULT 0,"
                                   "partial INTEGER NOT NULL DEFAULT 0,"
                                   "error_category TEXT NOT NULL DEFAULT 'None',"
                                   "PRIMARY KEY(conversation_id, message_id),"
                                   "FOREIGN KEY(conversation_id) REFERENCES conversations(id))"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }

    const QList<QPair<QString, QString>> messageColumns{
        {QStringLiteral("attachments_json"), QStringLiteral("TEXT NOT NULL DEFAULT '[]'")},
        {QStringLiteral("provider_id"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("model_id"), QStringLiteral("TEXT NOT NULL DEFAULT ''")},
        {QStringLiteral("reply_to_id"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("replaces_id"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("partial"), QStringLiteral("INTEGER NOT NULL DEFAULT 0")},
        {QStringLiteral("error_category"), QStringLiteral("TEXT NOT NULL DEFAULT 'None'")},
    };
    for (const auto& column : messageColumns) {
        if (!tableHasColumn(database_, QStringLiteral("conversation_messages"), column.first) &&
            !query.exec(QStringLiteral("ALTER TABLE conversation_messages ADD COLUMN %1 %2")
                            .arg(column.first, column.second))) {
            setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
            return;
        }
    }

    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conversation_summary_metadata("
                                   "conversation_id TEXT PRIMARY KEY NOT NULL,"
                                   "summary_timestamp TEXT NOT NULL,"
                                   "covered_first_message_id INTEGER NOT NULL DEFAULT 0,"
                                   "covered_last_message_id INTEGER NOT NULL DEFAULT 0,"
                                   "estimated_reduction_percent INTEGER NOT NULL DEFAULT 0,"
                                   "readiness_state TEXT NOT NULL,"
                                   "summary_text TEXT NOT NULL DEFAULT '',"
                                   "summary TEXT NOT NULL,"
                                   "FOREIGN KEY(conversation_id) REFERENCES conversations(id))"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }

    if (!database_.record(QStringLiteral("conversation_summary_metadata"))
             .contains(QStringLiteral("summary_text"))) {
        if (!query.exec(QStringLiteral("ALTER TABLE conversation_summary_metadata "
                                       "ADD COLUMN summary_text TEXT NOT NULL DEFAULT ''"))) {
            setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
            return;
        }
    }

    if (!query.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS conversation_schema_metadata("
                                   "key TEXT PRIMARY KEY NOT NULL,"
                                   "value INTEGER NOT NULL)"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }

    query.prepare(QStringLiteral("INSERT INTO conversation_schema_metadata(key, value) "
                                 "VALUES(?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
    query.addBindValue(QStringLiteral("schema_version"));
    query.addBindValue(currentSchemaVersion);
    if (!query.exec()) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }

    if (!query.exec(QStringLiteral("UPDATE conversation_messages SET status='interrupted' "
                                   "WHERE status IN ('sending','streaming')"))) {
        setLastError(ConversationStoreErrorCode::StorageFailure, query.lastError().text());
        return;
    }
    setLastError(ConversationStoreErrorCode::None, {});
}

void SQLiteConversationStore::setLastError(ConversationStoreErrorCode code,
                                           const QString& summary) const {
    lastError_.code = code;
    lastError_.summary = summary;
}

} // namespace sentinel::core
