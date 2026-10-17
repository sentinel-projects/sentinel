#pragma once

#include "sentinel/core/model/ModelRouting.h"

namespace sentinel::core {

enum class ProviderProtocol {
    Ollama,
    OpenAiCompatible,
    AnthropicMessages,
    GeminiGenerateContent,
    SelectedCloud
};
enum class ProviderEndpoint {
    Ollama,
    LmStudio,
    LlamaCpp,
    FixedLocal,
    OpenAi,
    Anthropic,
    Gemini,
    DeepSeek,
    Groq,
    Mistral,
    SelectedCloud
};
enum class ProviderAdapter { Ollama, SelectedEndpoint };
enum class ProviderDiscovery {
    None,
    OllamaTags,
    OpenAiLocalModels,
    LlamaCppModels,
    OpenAiCloudModels,
    AnthropicModels,
    GeminiModels,
    SelectedCloud
};

struct ProviderRegistration {
    QString id;
    QString displayName;
    ProviderKind kind;
    ProviderProtocol protocol;
    ProviderEndpoint endpoint;
    ProviderAdapter adapter;
    ProviderDiscovery discovery;
    QString baseEndpoint;
    ModelCapabilities declaredCapabilities;
};

class ProviderRegistry final {
public:
    explicit ProviderRegistry(QList<ProviderRegistration> descriptors);
    static const ProviderRegistry& builtins();
    const QList<ProviderRegistration>& descriptors() const;
    const ProviderRegistration* find(const QString& id) const;
    const ProviderRegistration* endpointDescriptor(const QString& id,
                                                   const QString& selectedCloud) const;
    ProviderDiscovery discoveryStrategy(const QString& id, const QString& selectedCloud = {}) const;

private:
    const QList<ProviderRegistration> descriptors_;
};

} // namespace sentinel::core
