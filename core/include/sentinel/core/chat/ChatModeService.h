// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/chat/ChatSession.h"
#include "sentinel/core/chat/IConversationStore.h"
#include "sentinel/core/context/ContextBudgetManager.h"
#include "sentinel/core/model/ModelService.h"

#include "sentinel/core/interfaces/IMemoryStore.h"
#include <QObject>
#include <QSet>
#include <QThread>
#include <atomic>
#include <functional>
#include <memory>

namespace sentinel::core {

enum class ChatAttachmentState { Selected, Processing, Ready, Rejected };

struct ChatAttachment {
    QString id;
    QString fileName;
    QString mimeType;
    qint64 sizeBytes = 0;
    QString localReference;
    ChatAttachmentState state = ChatAttachmentState::Selected;
    QString compatibilityReason;
    QString text;
    QByteArray imageBytes;
};

class ChatModeService final : public QObject {
    Q_OBJECT
public:
    ChatModeService(ModelService& models, ChatSession& session, IConversationStore& store,
                    QObject* parent = nullptr);
    ~ChatModeService() override;
    bool send(const QString& conversationId, const QString& text,
              const QList<ChatAttachment>& attachments = {}, const ModelSelection& selection = {},
              bool requireLocal = false);
    bool regenerate(const QString& conversationId, int userMessageId,
                    const ModelSelection& selection = {}, bool requireLocal = false);
    bool retry(const QString& conversationId, int assistantMessageId,
               const ModelSelection& selection = {}, bool requireLocal = false);
    bool sendExisting(const QString& conversationId, int userMessageId, int replacesMessageId = 0,
                      const ModelSelection& selection = {}, bool requireLocal = false);
    void setResponseProfileInstructions(QString instructions) { profileInstructions_ = instructions.left(2000); }
    void setScopedMemory(const IMemoryStore* store,
                         std::function<MemoryAccessContext(const QString&)> context) {
        memory_ = store;
        memoryContext_ = std::move(context);
    }
    QJsonObject budgetDiagnostics() const {
        return budgetDiagnostics_;
    }
    bool stop();
    bool tryShutdown(int timeoutMs);
    bool busy() const;
    ChatMessage activeMessage() const;
    ChatProviderErrorCategory lastError() const;

signals:
    void messagesChanged();
    void requestStateChanged();

private:
    bool runTurn(const QString& conversationId, int userMessageId, int replacesMessageId,
                 const ModelSelection& selection = {}, bool requireLocal = false);
    void acceptDelta(const QString& conversationId, int messageId, const QString& delta);
    void acceptResult(const QString& conversationId, int messageId, const ChatProviderReply& reply);
    bool persist(const QString& conversationId, const ChatMessage& message);
    ContextBudgetResult contextFor(const QString& conversationId, int userMessageId,
                                   const ModelBinding& binding,
                                   const ChatRequestOptions& options) const;
    static ChatProviderErrorCategory bindingError(ModelBindingError error);

    QJsonObject budgetDiagnostics_;
    const IMemoryStore* memory_ = nullptr;
    std::function<MemoryAccessContext(const QString&)> memoryContext_;
    ModelService& models_;
    ChatSession& session_;
    IConversationStore& store_;
    bool stopping_ = false;
    QThread* worker_ = nullptr;
    QSet<QThread*> workers_;
    std::shared_ptr<std::atomic_bool> cancellation_;
    QString profileInstructions_;
    QString activeConversationId_;
    int activeMessageId_ = 0;
    qint64 lastPersistMs_ = 0;
    ChatProviderErrorCategory lastError_ = ChatProviderErrorCategory::None;
};

} // namespace sentinel::core
