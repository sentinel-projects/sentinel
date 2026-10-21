// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/app/StorageAdmission.h"
#include "sentinel/core/chat/IConversationStore.h"

#include <QSqlDatabase>

namespace sentinel::core {

class SQLiteConversationStore final : public IConversationStore {
public:
    explicit SQLiteConversationStore(QString databasePath,
                                     std::shared_ptr<StorageAdmission> admission = {},
                                     std::uint64_t generation = 0);
    bool coordinatedBy(const std::shared_ptr<StorageAdmission>& authority) const override {
        return authority && authority == admission_;
    }
    ~SQLiteConversationStore() override;

    SQLiteConversationStore(const SQLiteConversationStore&) = delete;
    SQLiteConversationStore& operator=(const SQLiteConversationStore&) = delete;
    SQLiteConversationStore(SQLiteConversationStore&&) = delete;
    SQLiteConversationStore& operator=(SQLiteConversationStore&&) = delete;

    ConversationRecord createConversation(const QString& title) override;
    QList<ConversationRecord> listConversations() const override;
    QList<ConversationRecord> searchConversations(const QString& query,
                                                  int limit = 50) const override;
    bool appendMessage(const ConversationMessageRecord& message) override;
    QList<ConversationMessageRecord> loadMessages(const QString& conversationId) const override;
    bool renameConversation(const QString& conversationId, const QString& title) override;
    bool autoTitleConversation(const QString& conversationId, const QString& title) override;
    bool updateAutoTitleConversation(const QString& conversationId, const QString& title,
                                     const QString& expectedTitle) override;
    bool archiveConversation(const QString& conversationId) override;
    bool unarchiveConversation(const QString& conversationId) override;
    bool pinConversation(const QString& conversationId) override;
    bool unpinConversation(const QString& conversationId) override;
    bool deleteConversation(const QString& conversationId) override;
    bool discardImportedConversation(const QString& conversationId) override;
    bool clearHistory() override;
    int pruneCompletedBefore(const QDateTime& cutoffUtc, int limit = 100) override;
    bool saveSummaryMetadata(const ConversationSummaryMetadataRecord& metadata) override;
    ConversationSummaryMetadataRecord
    loadSummaryMetadata(const QString& conversationId) const override;
    ConversationStoreStatus status() const override;
    ConversationStoreError lastError() const override;

    QString databasePath() const;
    int schemaVersion() const;

private:
    static QString normalizedTitle(const QString& title);
    bool conversationExists(const QString& conversationId) const;
    bool conversationArchived(const QString& conversationId) const;
    bool updateConversationMetadata(const QString& conversationId, bool archived, bool deleted);
    bool updatePinnedMetadata(const QString& conversationId, bool pinned);
    void open();
    void initializeSchema();
    void setLastError(ConversationStoreErrorCode code, const QString& summary) const;

    static constexpr int currentSchemaVersion = 6;

    StorageAdmission::Writer admitWrite();
    std::shared_ptr<StorageAdmission> admission_;
    std::uint64_t generation_ = 0;
    QString databasePath_;
    QString connectionName_;
    QSqlDatabase database_;
    mutable ConversationStoreError lastError_;
};

} // namespace sentinel::core
