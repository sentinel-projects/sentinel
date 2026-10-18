#include "sentinel/core/provider/ProviderRegistry.h"

#include <QSet>
#include <stdexcept>
#include <utility>

namespace sentinel::core {
namespace {
ModelCapabilities declaredCapabilities(bool nativeTools) {
    ModelCapabilities capabilities;
    capabilities.streaming = CapabilitySupport::Supported;
    capabilities.nativeToolCalling =
        nativeTools ? CapabilitySupport::Supported : CapabilitySupport::Unsupported;
    capabilities.structuredOutput =
        nativeTools ? CapabilitySupport::Unknown : CapabilitySupport::Unsupported;
    capabilities.provenance.streaming = ModelMetadataSource::ProviderDefault;
    capabilities.provenance.nativeToolCalling = ModelMetadataSource::ProviderDefault;
    if (!nativeTools) {
        capabilities.provenance.structuredOutput = ModelMetadataSource::ProviderDefault;
    }
    return capabilities;
}
} // namespace

ProviderRegistry::ProviderRegistry(QList<ProviderRegistration> descriptors)
    : descriptors_(std::move(descriptors)) {
    QSet<QString> identities;
    for (const auto& descriptor : descriptors_) {
        if (descriptor.id.isEmpty() || descriptor.id != descriptor.id.trimmed().toLower() ||
            identities.contains(descriptor.id)) {
            throw std::invalid_argument("Provider descriptors require unique canonical IDs");
        }
        identities.insert(descriptor.id);
    }
}

const ProviderRegistry& ProviderRegistry::builtins() {
    using Protocol = ProviderProtocol;
    using Endpoint = ProviderEndpoint;
    using Adapter = ProviderAdapter;
    using Discovery = ProviderDiscovery;
    using Kind = ProviderKind;
    static const ProviderRegistry registry({
        {"ollama",
         "Ollama",
         Kind::Local,
         Protocol::Ollama,
         Endpoint::Ollama,
         Adapter::Ollama,
         Discovery::OllamaTags,
         {},
         declaredCapabilities(false)},
        {"lm-studio",
         "LM Studio",
         Kind::Local,
         Protocol::OpenAiCompatible,
         Endpoint::LmStudio,
         Adapter::SelectedEndpoint,
         Discovery::OpenAiLocalModels,
         {},
         declaredCapabilities(false)},
        {"openai-compatible-local", "OpenAI-compatible local endpoint", Kind::Local,
         Protocol::OpenAiCompatible, Endpoint::FixedLocal, Adapter::SelectedEndpoint,
         Discovery::OpenAiLocalModels, "http://127.0.0.1:8000", declaredCapabilities(false)},
        {"llama-cpp-server",
         "llama.cpp server",
         Kind::Local,
         Protocol::OpenAiCompatible,
         Endpoint::LlamaCpp,
         Adapter::SelectedEndpoint,
         Discovery::LlamaCppModels,
         {},
         declaredCapabilities(false)},
        {"cloud-api",
         "Cloud API",
         Kind::Cloud,
         Protocol::SelectedCloud,
         Endpoint::SelectedCloud,
         Adapter::SelectedEndpoint,
         Discovery::SelectedCloud,
         {},
         declaredCapabilities(false)},
        {"openai", "OpenAI", Kind::Cloud, Protocol::OpenAiCompatible, Endpoint::OpenAi,
         Adapter::SelectedEndpoint, Discovery::OpenAiCloudModels, "https://api.openai.com",
         declaredCapabilities(false)},
        {"openai-compatible", "OpenAI-compatible", Kind::Cloud, Protocol::OpenAiCompatible,
         Endpoint::OpenAi, Adapter::SelectedEndpoint, Discovery::None, "https://api.openai.com",
         declaredCapabilities(false)},
        {"claude", "Claude", Kind::Cloud, Protocol::AnthropicMessages, Endpoint::Anthropic,
         Adapter::SelectedEndpoint, Discovery::AnthropicModels, "https://api.anthropic.com",
         declaredCapabilities(true)},
        {"gemini", "Gemini", Kind::Cloud, Protocol::GeminiGenerateContent, Endpoint::Gemini,
         Adapter::SelectedEndpoint, Discovery::GeminiModels,
         "https://generativelanguage.googleapis.com", declaredCapabilities(true)},
        {"deepseek", "DeepSeek", Kind::Cloud, Protocol::OpenAiCompatible, Endpoint::DeepSeek,
         Adapter::SelectedEndpoint, Discovery::OpenAiCloudModels, "https://api.deepseek.com",
         declaredCapabilities(false)},
        {"groq", "Groq", Kind::Cloud, Protocol::OpenAiCompatible, Endpoint::Groq,
         Adapter::SelectedEndpoint, Discovery::OpenAiCloudModels, "https://api.groq.com/openai",
         declaredCapabilities(false)},
        {"mistral", "Mistral", Kind::Cloud, Protocol::OpenAiCompatible, Endpoint::Mistral,
         Adapter::SelectedEndpoint, Discovery::OpenAiCloudModels, "https://api.mistral.ai",
         declaredCapabilities(false)},
    });
    return registry;
}

const QList<ProviderRegistration>& ProviderRegistry::descriptors() const {
    return descriptors_;
}

const ProviderRegistration* ProviderRegistry::find(const QString& id) const {
    for (const auto& descriptor : descriptors_) {
        if (descriptor.id == id) {
            return &descriptor;
        }
    }
    return nullptr;
}

const ProviderRegistration*
ProviderRegistry::endpointDescriptor(const QString& id, const QString& selectedCloud) const {
    const auto* descriptor = find(id);
    if (descriptor && descriptor->endpoint == ProviderEndpoint::SelectedCloud) {
        descriptor = find(selectedCloud.trimmed().toLower());
        if (!descriptor || descriptor->kind != ProviderKind::Cloud ||
            descriptor->endpoint == ProviderEndpoint::SelectedCloud) {
            return find(QStringLiteral("openai"));
        }
    }
    return descriptor;
}

ProviderDiscovery ProviderRegistry::discoveryStrategy(const QString& id,
                                                      const QString& selectedCloud) const {
    const auto* descriptor = find(id);
    if (descriptor && descriptor->discovery == ProviderDiscovery::SelectedCloud) {
        descriptor = find(selectedCloud.trimmed().toLower());
        if (!descriptor || descriptor->kind != ProviderKind::Cloud ||
            descriptor->discovery == ProviderDiscovery::SelectedCloud) {
            return ProviderDiscovery::None;
        }
    }
    return descriptor ? descriptor->discovery : ProviderDiscovery::None;
}

} // namespace sentinel::core
