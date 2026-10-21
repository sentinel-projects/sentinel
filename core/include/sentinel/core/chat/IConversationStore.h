// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/chat/ChatMessage.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace sentinel::core {

enum class ConversationStoreStatus {
    Ready,
    Unavailable,
};

inline QString conversationStoreStatusName(ConversationStoreStatus status) {
    switch (status) {
    case ConversationStoreStatus::Ready:
        return QStringLiteral("Ready");
    case ConversationStoreStatus::Unavailable:
        return QStringLiteral("Unavailable");
    }

    return QStringLiteral("Unavailable");
}

enum class ConversationStoreErrorCode {
    None,
    Unavailable,
    InvalidConversationId,
    InvalidTitle,
    StorageFailure,
    UnsupportedOperation,
    UnsupportedSchema,
};

struct ConversationStoreError {
    ConversationStoreErrorCode code = ConversationStoreErrorCode::None;
    QString summary;
};

struct ConversationRecord {
    QString id;
    QString title = QStringLiteral("Untitled Conversation");
    QDateTime createdAtUtc;
    QDateTime updatedAtUtc;
    bool archived = false;
    bool pinned = false;
    bool deleted = false;
    bool userRenamed = false;
    int messageCount = 0;
    QString summary = QStringLiteral("Conversation metadata is not available.");
};

inline QString conversationRecordSummary(const ConversationRecord& record) {
    return QStringLiteral("%1 / %2 %3 / %4")
        .arg(record.title)
        .arg(record.messageCount)
        .arg(record.messageCount == 1 ? QStringLiteral("message") : QStringLiteral("messages"))
        .arg(record.archived ? QStringLiteral("Archived")
             : record.pinned ? QStringLiteral("Pinned")
                             : QStringLiteral("Active"));
}

struct ConversationMessageRecord {
    QString conversationId;
    int messageId = 0;
    ChatRole role = ChatRole::System;
    QString content;
    QDateTime timestampUtc;
    ChatMessageStatus status = ChatMessageStatus::Received;
    QString providerId;
    QString modelId;
    int replyToMessageId = 0;
    int replacesMessageId = 0;
    bool partial = false;
    ChatProviderErrorCategory errorCategory = ChatProviderErrorCategory::None;
    QString attachmentsJson;
};

struct ConversationSummaryMetadataRecord {
    QString conversationId;
    QDateTime summaryTimestampUtc;
    int coveredFirstMessageId = 0;
    int coveredLastMessageId = 0;
    int estimatedReductionPercent = 0;
    QString readinessState = QStringLiteral("Blocked");
    QString summaryText;
    QString summary = QStringLiteral("No conversation summary metadata has been persisted.");
};

class IConversationStore {
public:
    Q_DISABLE_COPY(IConversationStore)
    IConversationStore() = default;
    virtual ~IConversationStore() = default;
    virtual bool coordinatedBy(const std::shared_ptr<StorageAdmission>&) const {
        return false;
    }

    virtual ConversationRecord createConversation(const QString& title) = 0;
    virtual QList<ConversationRecord> listConversations() const = 0;
    virtual QList<ConversationRecord> searchConversations(const QString& query,
                                                           int limit = 50) const {
        Q_UNUSED(query)
        Q_UNUSED(limit)
        return {};
    }
    virtual bool appendMessage(const ConversationMessageRecord& message) = 0;
    virtual QList<ConversationMessageRecord> loadMessages(const QString& conversationId) const = 0;
    virtual bool renameConversation(const QString& conversationId, const QString& title) = 0;
    virtual bool autoTitleConversation(const QString& conversationId, const QString& title) {
        Q_UNUSED(conversationId)
        Q_UNUSED(title)
        return false;
    }
    // Compare-and-set an automatic title without overwriting user edits or stale jobs.
    virtual bool updateAutoTitleConversation(const QString& conversationId, const QString& title,
                                             const QString& expectedTitle) {
        Q_UNUSED(conversationId)
        Q_UNUSED(title)
        Q_UNUSED(expectedTitle)
        return false;
    }
    virtual bool archiveConversation(const QString& conversationId) = 0;
    virtual bool unarchiveConversation(const QString& conversationId) = 0;
    virtual bool pinConversation(const QString& conversationId) = 0;
    virtual bool unpinConversation(const QString& conversationId) = 0;
    virtual bool deleteConversation(const QString& conversationId) = 0;
    // Import rollback only: permanently remove a conversation created by this import.
    virtual bool discardImportedConversation(const QString& conversationId) {
        Q_UNUSED(conversationId)
        return false;
    }
    virtual bool clearHistory() { return false; }
    virtual int pruneCompletedBefore(const QDateTime& cutoffUtc, int limit = 100) {
        Q_UNUSED(cutoffUtc)
        Q_UNUSED(limit)
        return -1;
    }
    virtual bool saveSummaryMetadata(const ConversationSummaryMetadataRecord& metadata) {
        Q_UNUSED(metadata);
        return false;
    }
    virtual ConversationSummaryMetadataRecord
    loadSummaryMetadata(const QString& conversationId) const {
        Q_UNUSED(conversationId);
        return {};
    }

    virtual ConversationStoreStatus status() const {
        return ConversationStoreStatus::Ready;
    }
    virtual ConversationStoreError lastError() const {
        return {};
    }
};

} // namespace sentinel::core
