// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/runtime/OllamaRuntime.h"
#include <optional>

namespace sentinel::core {

// Real IChatProvider backed by a local Ollama runtime. sendMessage() performs a
// real /api/generate request through OllamaLocalInferenceClient; status() reflects
// live Ollama health and installed-model discovery instead of a hardcoded state.
class OllamaChatProvider final : public IChatProvider {
public:
    explicit OllamaChatProvider(OllamaConfig config = OllamaConfig{}, int timeoutMs = 30000);

    QString name() const override;
    ChatProviderStatus status() const override;
    ChatProviderConcurrency concurrency() const override {
        return ChatProviderConcurrency::Supported;
    }
    ChatProviderReply sendMessage(const QString& message) override;
    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override;
    bool supportsStreaming() const override {
        return true;
    }
    bool supportsBoundedStreaming() const override {
        return true;
    }
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken) override;
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken,
                         const ChatRequestOptions& options) override;

    QString endpoint() const;
    QString selectedModel() const;
    void setSelectedModel(const QString& model);
    void setDiscoverySnapshot(const OllamaModelDiscoveryResult& discovery) {
        discoverySnapshot_ = discovery;
    }

private:
    ChatProviderReply streamMessage(const QString& message,
                                    const std::function<void(const QString&)>& onDelta,
                                    const std::shared_ptr<std::atomic_bool>& cancellationToken,
                                    const QList<ChatImage>& images, int deadlineMs,
                                    int maxOutputTokens, bool requireBoundedStreaming,
                                    ReasoningControl reasoning = ReasoningControl::Default);
    ChatProviderReply sendMessageWithToken(
        const QString& message, const std::shared_ptr<std::atomic_bool>& cancellationToken,
        const QList<ChatImage>& images = {}, int deadlineMs = 0, int maxOutputTokens = 0,
        ReasoningControl reasoning = ReasoningControl::Default);
    OllamaConfig config_;
    int timeoutMs_ = 30000;
    QString selectedModel_;
    std::optional<OllamaModelDiscoveryResult> discoverySnapshot_;
};

} // namespace sentinel::core
