// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once
#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/chat/ChatMessage.h"

#include <QList>
#include <QString>

namespace sentinel::core {

class IChatHistoryStore {
public:
    virtual ~IChatHistoryStore() = default;
    virtual bool coordinatedBy(const std::shared_ptr<StorageAdmission>&) const {
        return false;
    }

    virtual QList<ChatMessage> loadMessages() const = 0;
    virtual QList<ChatMessage> recentMessages(int limit) const {
        Q_UNUSED(limit)
        return {};
    }
    virtual QList<ChatMessage> searchMessages(const QString& query, int limit,
                                               int beforeId = 0) const {
        Q_UNUSED(query)
        Q_UNUSED(limit)
        Q_UNUSED(beforeId)
        return {};
    }
    virtual void appendMessage(const ChatMessage& message) = 0;
    virtual void clear() = 0;
    virtual int pruneCompletedBefore(const QDateTime& cutoffUtc, int limit = 500) {
        Q_UNUSED(cutoffUtc)
        Q_UNUSED(limit)
        return -1;
    }
    virtual bool isAvailable() const {
        return true;
    }
    virtual QString lastError() const {
        return {};
    }
};

} // namespace sentinel::core
