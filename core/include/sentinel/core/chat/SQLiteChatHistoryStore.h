// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/chat/IChatHistoryStore.h"

#include <QSqlDatabase>
#include <QString>

namespace sentinel::core {

class SQLiteChatHistoryStore final : public IChatHistoryStore {
public:
    explicit SQLiteChatHistoryStore(QString databasePath,
                                    std::shared_ptr<StorageAdmission> admission = {},
                                    std::uint64_t generation = 0);
    ~SQLiteChatHistoryStore() override;

    SQLiteChatHistoryStore(const SQLiteChatHistoryStore&) = delete;
    SQLiteChatHistoryStore& operator=(const SQLiteChatHistoryStore&) = delete;
    SQLiteChatHistoryStore(SQLiteChatHistoryStore&&) = delete;
    SQLiteChatHistoryStore& operator=(SQLiteChatHistoryStore&&) = delete;

    QList<ChatMessage> loadMessages() const override;
    QList<ChatMessage> recentMessages(int limit) const override;
    QList<ChatMessage> searchMessages(const QString& query, int limit,
                                       int beforeId = 0) const override;
    void appendMessage(const ChatMessage& message) override;
    void clear() override;
    int pruneCompletedBefore(const QDateTime& cutoffUtc, int limit = 500) override;
    bool isAvailable() const override;
    bool coordinatedBy(const std::shared_ptr<StorageAdmission>& authority) const override {
        return authority && admission_ == authority;
    }
    QString lastError() const override;

    QString databasePath() const;
    int schemaVersion() const;

private:
    void open();
    void initializeSchema();
    void setLastError(QString error) const;

    static constexpr int currentSchemaVersion = 3;

    StorageAdmission::Writer admitWrite();
    std::shared_ptr<StorageAdmission> admission_;
    const std::uint64_t generation_;
    QString databasePath_;
    QString connectionName_;
    QSqlDatabase database_;
    bool ftsReady_ = false;
    mutable QString lastError_;
};

} // namespace sentinel::core
