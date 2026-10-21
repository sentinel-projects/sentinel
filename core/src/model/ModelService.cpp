// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/model/ModelService.h"

#include "sentinel/core/app/AppSettings.h"
#include "sentinel/core/chat/OllamaChatProvider.h"
#include "sentinel/core/memory/JsonSettingsStore.h"
#include "sentinel/core/model/IModelRouter.h"
#include "sentinel/core/provider/ProviderRegistry.h"
#include "sentinel/core/provider/SelectedEndpointChatProvider.h"
#include "sentinel/core/runtime/LocalInference.h"

#include <QDir>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>

namespace sentinel::core {

struct ProviderHealthRegistry {
    struct Entry {
        ProviderHealth health = ProviderHealth::Unknown;
        quint64 sequence = 0;
        QString source;
    };
    std::mutex mutex;
    std::atomic<quint64> nextSequence{0};
    QHash<QString, Entry> entries;
    OllamaModelDiscoveryResult ollamaDiscovery;
    QList<OllamaModelSummary> ollamaModels;
    quint64 ollamaDiscoverySequence = 0;
    struct Catalog {
        QList<OllamaModelSummary> models;
        ProviderDiscoveryOutcome outcome;
        quint64 sequence = 0;
        bool observed = false;
        bool nativeCatalog = false;
    };
    QHash<QString, Catalog> catalogs;
};

QString providerHealthName(ProviderHealth health) {
    switch (health) {
    case ProviderHealth::Unknown:
        return QStringLiteral("Unknown");
    case ProviderHealth::Available:
        return QStringLiteral("Available");
    case ProviderHealth::Degraded:
        return QStringLiteral("Degraded");
    case ProviderHealth::Unavailable:
        return QStringLiteral("Unavailable");
    }
    return QStringLiteral("Unknown");
}

QString providerCatalogStateName(ProviderCatalogState state) {
    switch (state) {
    case ProviderCatalogState::Pending:
        return QStringLiteral("Pending");
    case ProviderCatalogState::Available:
        return QStringLiteral("Available");
    case ProviderCatalogState::Empty:
        return QStringLiteral("Empty catalog");
    case ProviderCatalogState::Failed:
        return QStringLiteral("Discovery failed");
    case ProviderCatalogState::AuthenticationRequired:
        return QStringLiteral("Authentication required");
    case ProviderCatalogState::EndpointUnavailable:
        return QStringLiteral("Endpoint unavailable");
    case ProviderCatalogState::Stale:
        return QStringLiteral("Stale catalog");
    case ProviderCatalogState::ConfiguredModelOnly:
        return QStringLiteral("Configured model only");
    case ProviderCatalogState::Unverified:
        return QStringLiteral("Unverified");
    }
    return QStringLiteral("Unverified");
}

namespace {
class HealthTrackedProvider final : public IChatProvider {
public:
    HealthTrackedProvider(QString id, std::shared_ptr<IChatProvider> delegate,
                          std::shared_ptr<ProviderHealthRegistry> registry)
        : id_(id.trimmed().toLower()), delegate_(std::move(delegate)),
          registry_(std::move(registry)) {}
    QString name() const override {
        return delegate_->name();
    }
    ChatProviderStatus status() const override {
        return delegate_->status();
    }
    ChatProviderConcurrency concurrency() const override {
        return delegate_->concurrency();
    }
    bool supportsStreaming() const override {
        return delegate_->supportsStreaming();
    }
    bool supportsBoundedStreaming() const override {
        return delegate_->supportsBoundedStreaming();
    }
    ChatProviderReply sendMessage(const QString& message) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendMessage(message);
        update(sequence, reply, QStringLiteral("generation"));
        return reply;
    }
    ChatProviderReply sendRequest(const QString& message,
                                  const ChatRequestOptions& options) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendRequest(message, options);
        update(sequence, reply, QStringLiteral("generation"));
        return reply;
    }
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendMessageStreaming(message, onDelta, cancellationToken);
        update(sequence, reply, QStringLiteral("stream"));
        return reply;
    }
    ChatProviderReply
    sendMessageStreaming(const QString& message, const std::function<void(const QString&)>& onDelta,
                         const std::shared_ptr<std::atomic_bool>& cancellationToken,
                         const ChatRequestOptions& options) override {
        const auto sequence = ++registry_->nextSequence;
        auto reply = delegate_->sendMessageStreaming(message, onDelta, cancellationToken, options);
        update(sequence, reply, QStringLiteral("stream"));
        return reply;
    }

private:
    void update(quint64 sequence, const ChatProviderReply& reply, const QString& source) {
        std::lock_guard lock(registry_->mutex);
        auto& entry = registry_->entries[id_];
        if (sequence < entry.sequence)
            return;
        entry.sequence = sequence;
        entry.source = source;
        if (reply.success)
            entry.health = ProviderHealth::Available;
        else if (reply.category == ChatProviderErrorCategory::RateLimited ||
                 reply.category == ChatProviderErrorCategory::ConnectionFailed ||
                 reply.category == ChatProviderErrorCategory::Timeout ||
                 reply.category == ChatProviderErrorCategory::MalformedResponse)
            entry.health = ProviderHealth::Degraded;
        else if (reply.category == ChatProviderErrorCategory::ProviderUnavailable ||
                 reply.category == ChatProviderErrorCategory::AuthenticationRequired)
            entry.health = ProviderHealth::Unavailable;
    }
    QString id_;
    std::shared_ptr<IChatProvider> delegate_;
    std::shared_ptr<ProviderHealthRegistry> registry_;
};

QString normalizedProviderId(const QString& providerId) {
    return providerId.trimmed().toLower();
}

bool isCloudProviderId(const QString& providerId) {
    const auto* descriptor = ProviderRegistry::builtins().find(providerId);
    return descriptor && descriptor->kind == ProviderKind::Cloud;
}

QString defaultProviderId() {
    return QStringLiteral("ollama");
}

ModelCapabilities mergedCapabilities(ModelCapabilities base, const ModelCapabilities& override,
                                     ModelMetadataSource source) {
    auto merge = [source](CapabilitySupport& target, ModelMetadataSource& provenance,
                          CapabilitySupport value, ModelMetadataSource suppliedSource) {
        if (value != CapabilitySupport::Unknown) {
            target = value;
            provenance = suppliedSource == ModelMetadataSource::Unknown ? source : suppliedSource;
        }
    };
    merge(base.streaming, base.provenance.streaming, override.streaming,
          override.provenance.streaming);
    merge(base.structuredOutput, base.provenance.structuredOutput, override.structuredOutput,
          override.provenance.structuredOutput);
    merge(base.nativeToolCalling, base.provenance.nativeToolCalling, override.nativeToolCalling,
          override.provenance.nativeToolCalling);
    merge(base.combinedToolsAndStructuredOutput, base.provenance.combinedToolsAndStructuredOutput,
          override.combinedToolsAndStructuredOutput,
          override.provenance.combinedToolsAndStructuredOutput);
    merge(base.visionInput, base.provenance.visionInput, override.visionInput,
          override.provenance.visionInput);
    merge(base.audioInput, base.provenance.audioInput, override.audioInput,
          override.provenance.audioInput);
    merge(base.audioOutput, base.provenance.audioOutput, override.audioOutput,
          override.provenance.audioOutput);
    if (override.contextWindow && *override.contextWindow > 0) {
        base.contextWindow = override.contextWindow;
        base.provenance.contextWindow =
            override.provenance.contextWindow == ModelMetadataSource::Unknown
                ? source
                : override.provenance.contextWindow;
    }
    if (override.maxOutputTokens && *override.maxOutputTokens > 0) {
        base.maxOutputTokens = override.maxOutputTokens;
        base.provenance.maxOutputTokens =
            override.provenance.maxOutputTokens == ModelMetadataSource::Unknown
                ? source
                : override.provenance.maxOutputTokens;
    }
    return base;
}

} // namespace

QString modelBindingErrorName(ModelBindingError error) {
    switch (error) {
    case ModelBindingError::None:
        return QStringLiteral("None");
    case ModelBindingError::ProviderNotFound:
        return QStringLiteral("ProviderNotFound");
    case ModelBindingError::ProviderUnavailable:
        return QStringLiteral("ProviderUnavailable");
    case ModelBindingError::ModelNotFound:
        return QStringLiteral("ModelNotFound");
    case ModelBindingError::ConfigurationInvalid:
        return QStringLiteral("ConfigurationInvalid");
    case ModelBindingError::AuthenticationRequired:
        return QStringLiteral("AuthenticationRequired");
    }

    return QStringLiteral("None");
}

QString modelBindingFailureSummary(const ModelBindingResolution& resolution) {
    const auto providerId = resolution.binding.providerId.isEmpty() ? QStringLiteral("<none>")
                                                                    : resolution.binding.providerId;
    const auto modelId = resolution.binding.modelId.isEmpty() ? QStringLiteral("<none>")
                                                              : resolution.binding.modelId;
    return QStringLiteral("Model binding failed for provider '%1' / model '%2': %3 (%4)")
        .arg(providerId, modelId, resolution.reason, modelBindingErrorName(resolution.error));
}

ModelService::ModelService(AppSettings* settings, QObject* parent)
    : QObject(parent), settings_(settings),
      ollamaEndpoint_(OllamaEndpoint::defaultEndpoint().toString()),
      lmStudioEndpoint_(QStringLiteral("http://127.0.0.1:1234")),
      llamaCppEndpoint_(QStringLiteral("http://127.0.0.1:8080")) {
    providerHealthRegistry_ = std::make_shared<ProviderHealthRegistry>();
    providerHealthRegistry_->ollamaDiscovery.lifecycle = ChatRequestLifecycle::Pending;
    providerHealthRegistry_->ollamaDiscovery.errorCategory = ChatProviderErrorCategory::None;
    providerHealthRegistry_->ollamaDiscovery.safeDetail =
        QStringLiteral("Ollama model discovery pending.");
    if (settings_) {
        selection_.providerId = normalizedProviderId(settings_->selectedRuntimeProvider());
        selection_.modelId = settings_->selectedModelForProvider(selection_.providerId).trimmed();
        connect(settings_, &AppSettings::cloudApiKeysChanged, this, [this] {
            {
                std::lock_guard lock(providerHealthRegistry_->mutex);
                for (const auto& descriptor : ProviderRegistry::builtins().descriptors()) {
                    if (descriptor.kind == ProviderKind::Cloud) {
                        providerHealthRegistry_->catalogs.remove(descriptor.id);
                    }
                }
            }
            emit providerRegistryChanged();
        });
        connect(settings_, &AppSettings::selectedCloudProviderChanged, this, [this] {
            {
                std::lock_guard lock(providerHealthRegistry_->mutex);
                providerHealthRegistry_->catalogs.remove(QStringLiteral("cloud-api"));
            }
            emit providerRegistryChanged();
            emit modelCapabilitiesChanged();
        });
    } else {
        selection_.providerId = defaultProviderId();
    }

    for (const auto& descriptor : ProviderRegistry::builtins().descriptors()) {
        if (descriptor.adapter == ProviderAdapter::Ollama) {
            registerProvider(
                descriptor.id,
                [this](const ModelBinding& binding) {
                    auto provider = std::make_shared<OllamaChatProvider>(
                        OllamaConfig::fromEndpoint(ollamaEndpoint_), localInferenceTimeoutMs_);
                    provider->setSelectedModel(binding.modelId);
                    provider->setDiscoverySnapshot(ollamaDiscovery());
                    return std::shared_ptr<IChatProvider>(std::move(provider));
                },
                descriptor.declaredCapabilities);
        } else {
            registerProvider(
                descriptor.id,
                [this](const ModelBinding& binding) {
                    return makeSelectedEndpointChatProvider(binding, providerConfig(binding),
                                                            localInferenceTimeoutMs_);
                },
                descriptor.declaredCapabilities);
        }
    }
}

ModelService::~ModelService() = default;

void ModelService::setModelRouter(IModelRouter* router) {
    router_ = router;
}

void ModelService::registerProvider(const QString& providerId, ModelProviderFactory factory,
                                    ModelCapabilities defaults) {
    const auto normalized = normalizedProviderId(providerId);
    if (normalized.isEmpty() || !factory)
        return;
    providerFactories_.insert(normalized, std::move(factory));
    providerCapabilities_.insert(normalized, std::move(defaults));
    emit providerRegistryChanged();
}

void ModelService::setModelCapabilities(const QString& providerId, const QString& modelId,
                                        ModelCapabilities capabilities) {
    const auto provider = normalizedProviderId(providerId);
    const auto model = modelId.trimmed();
    if (!provider.isEmpty() && !model.isEmpty()) {
        modelCapabilities_[provider].insert(model, std::move(capabilities));
        emit modelCapabilitiesChanged();
    }
}

ModelCapabilities ModelService::capabilityOverrides(const QString& providerId,
                                                    const QString& modelId) const {
    return settings_ ? settings_->modelCapabilitiesOverride(providerId, modelId)
                     : ModelCapabilities{};
}

void ModelService::setCapabilityOverrides(const QString& providerId, const QString& modelId,
                                          const ModelCapabilities& overrides) {
    if (!settings_ || providerId.trimmed().isEmpty() || modelId.trimmed().isEmpty())
        return;
    settings_->setModelCapabilitiesOverride(providerId, modelId, overrides);
    emit modelCapabilitiesChanged();
}

void ModelService::clearCapabilityOverrides(const QString& providerId, const QString& modelId) {
    setCapabilityOverrides(providerId, modelId, {});
}

ModelCapabilities ModelService::capabilities(const QString& providerId,
                                             const QString& modelId) const {
    const auto provider = normalizedProviderId(providerId);
    const auto model = modelId.trimmed();
    auto result = mergedCapabilities({}, providerCapabilities_.value(provider),
                                     ModelMetadataSource::ProviderDefault);
    if (provider == QLatin1String("cloud-api")) {
        const auto host = providerConfig(ModelBinding{provider, model}).endpoint.host().toLower();
        if (host == QLatin1String("api.anthropic.com") ||
            host == QLatin1String("generativelanguage.googleapis.com")) {
            result.nativeToolCalling = CapabilitySupport::Supported;
            result.provenance.nativeToolCalling = ModelMetadataSource::ProviderDefault;
            result.structuredOutput = CapabilitySupport::Unknown;
            result.provenance.structuredOutput = ModelMetadataSource::Unknown;
        }
    }
    if (router_) {
        const auto route = router_->resolveSelection(ModelBinding{provider, model});
        if (route.status == ModelRoutingStatus::Routed) {
            if (route.provider.id == provider)
                result = mergedCapabilities(result, route.provider.modelCapabilities,
                                            ModelMetadataSource::ProviderOwnedMetadata);
            if (route.model.providerId == provider && route.model.id == model) {
                result = mergedCapabilities(result, route.model.capabilities,
                                            ModelMetadataSource::ProviderOwnedMetadata);
                if (route.model.contextWindowTokens > 0 && !result.contextWindow) {
                    result.contextWindow = route.model.contextWindowTokens;
                    result.provenance.contextWindow = ModelMetadataSource::ProviderOwnedMetadata;
                }
            }
        }
    }
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        const auto& models = provider == QLatin1String("ollama")
                                 ? providerHealthRegistry_->ollamaModels
                                 : providerHealthRegistry_->catalogs.value(provider).models;
        for (const auto& discovered : models) {
            if (discovered.name == model) {
                result = mergedCapabilities(
                    result, discovered.capabilities,
                    provider == QLatin1String("ollama") || provider == QLatin1String("lm-studio") ||
                            provider == QLatin1String("llama-cpp-server") ||
                            provider == QLatin1String("openai-compatible-local")
                        ? ModelMetadataSource::RuntimeReported
                        : ModelMetadataSource::ProviderCatalog);
                break;
            }
        }
    }
    result = mergedCapabilities(result, modelCapabilities_.value(provider).value(model),
                                ModelMetadataSource::ProgrammaticMetadata);
    if (settings_)
        result = mergedCapabilities(result, settings_->modelCapabilitiesOverride(provider, model),
                                    ModelMetadataSource::UserOverride);
    return result;
}

CurrentModelMetadata ModelService::currentModelMetadata(const QString& providerId,
                                                        const QString& modelId) const {
    CurrentModelMetadata metadata;
    metadata.providerId = normalizedProviderId(providerId);
    metadata.modelId = modelId.trimmed();
    metadata.providerKind =
        isCloudProviderId(metadata.providerId) ? ProviderKind::Cloud : ProviderKind::Local;
    metadata.catalog = providerStatus(metadata.providerId, metadata.modelId).catalog;
    metadata.capabilities = capabilities(metadata.providerId, metadata.modelId);
    const auto models = providerDiscoveredModels(metadata.providerId);
    for (const auto& discovered : models) {
        if (discovered.name == metadata.modelId) {
            metadata.family = discovered.family;
            metadata.publisher = discovered.publisher;
            metadata.architecture = discovered.architecture;
            break;
        }
    }
    return metadata;
}

bool ModelService::isKnownProvider(const QString& providerId) const {
    return providerFactories_.contains(normalizedProviderId(providerId));
}

QStringList ModelService::knownProviderIds() const {
    auto ids = providerFactories_.keys();
    ids.sort();
    return ids;
}

ModelSelection ModelService::selectedModel() const {
    return selection_;
}

void ModelService::setSelectedModel(const ModelSelection& selection) {
    if (settings_) {
        settings_->clearStorageWriteError();
    }
    setSelectedProviderId(selection.providerId);
    if (settings_ && !settings_->storageErrorCode().isEmpty()) {
        return;
    }
    setSelectedModelId(selection.modelId);
}

void ModelService::setSelectedProviderId(const QString& providerId) {
    const auto normalized = normalizedProviderId(providerId);
    const auto selected =
        providerFactories_.contains(normalized) ? normalized : defaultProviderId();
    if (selected == selection_.providerId)
        return;
    const auto previous = selection_.providerId;
    selection_.providerId =
        selected; // Guard synchronous settings callbacks against recursive submission.
    if (settings_) {
        settings_->clearStorageWriteError();
        settings_->setSelectedRuntimeProvider(selected);
        if (!settings_->storageErrorCode().isEmpty()) {
            selection_.providerId = previous;
            return;
        }
    }
    emit selectedModelChanged();
}
void ModelService::setSelectedModelId(const QString& modelId) {
    const auto normalized = modelId.trimmed();
    if (normalized == selection_.modelId)
        return;
    const auto previous = selection_.modelId;
    selection_.modelId = normalized;
    if (settings_ && !selection_.providerId.isEmpty()) {
        settings_->clearStorageWriteError();
        settings_->setSelectedModelForProvider(selection_.providerId, normalized);
        if (!settings_->storageErrorCode().isEmpty()) {
            selection_.modelId = previous;
            return;
        }
    }
    emit selectedModelChanged();
}

ModelBindingResolution ModelService::resolve(const ModelSelection& selection) {
    return resolve(selection.providerId, selection.modelId);
}

ModelBindingResolution ModelService::resolve(const QString& providerId, const QString& modelId) {
    ModelBindingResolution resolution;
    const auto provider = normalizedProviderId(providerId);
    const auto model = modelId.trimmed();
    resolution.binding = ModelBinding{provider, model};
    resolution.selection = ModelSelection{provider, model};

    if (provider.isEmpty() || !providerFactories_.contains(provider)) {
        resolution.error = ModelBindingError::ProviderNotFound;
        resolution.reason = provider.isEmpty()
                                ? QStringLiteral("No provider is configured for this request.")
                                : QStringLiteral("Provider '%1' is not configured.").arg(provider);
        return resolution;
    }
    if (provider == QLatin1String("ollama")) {
        const auto discovery = ollamaDiscovery();
        if (discovery.lifecycle != ChatRequestLifecycle::Pending &&
            discovery.lifecycle != ChatRequestLifecycle::Running && !discovery.succeeded()) {
            resolution.error = ModelBindingError::ProviderUnavailable;
            resolution.reason = discovery.safeDetail;
            return resolution;
        }
        if (discovery.succeeded() && discovery.models.isEmpty()) {
            resolution.error = ModelBindingError::ModelNotFound;
            resolution.reason = discovery.safeDetail;
            return resolution;
        }
        if (discovery.succeeded() && !model.isEmpty()) {
            const bool available =
                std::any_of(discovery.models.cbegin(), discovery.models.cend(),
                            [&](const OllamaModelSummary& item) { return item.name == model; });
            if (!available) {
                resolution.error = ModelBindingError::ModelNotFound;
                resolution.reason = QStringLiteral("Selected Ollama model is unavailable.");
                return resolution;
            }
        }
    }
    if (model.isEmpty()) {
        resolution.error = ModelBindingError::ModelNotFound;
        resolution.reason = QStringLiteral("Provider '%1' has no configured model.").arg(provider);
        return resolution;
    }
    if (router_) {
        const auto route = router_->resolveSelection(resolution.binding);
        if (route.status != ModelRoutingStatus::Routed) {
            resolution.error = ModelBindingError::ProviderUnavailable;
            resolution.reason =
                QStringLiteral("Provider '%1' has no available model route.").arg(provider);
            return resolution;
        }
    }
    resolution.binding.capabilities = capabilities(provider, model);
    resolution.binding.contextWindowTokens =
        resolution.binding.capabilities.contextWindow.value_or(0);
    if (provider != QLatin1String("ollama")) {
        const auto config = providerConfig(resolution.binding);
        if (isCloudProviderId(provider) && config.apiKey.trimmed().isEmpty()) {
            resolution.error = ModelBindingError::AuthenticationRequired;
            resolution.reason =
                QStringLiteral("Provider '%1' requires an API key that is not configured.")
                    .arg(provider);
            return resolution;
        }
        if (!config.isAllowedEndpoint()) {
            resolution.error = ModelBindingError::ConfigurationInvalid;
            resolution.reason =
                QStringLiteral("Provider '%1' is not configured with a usable endpoint.")
                    .arg(provider);
            return resolution;
        }
        const auto catalog = providerStatus(provider, model);
        if (catalog.catalog == ProviderCatalogState::AuthenticationRequired) {
            resolution.error = ModelBindingError::AuthenticationRequired;
            resolution.reason = catalog.safeDetail;
            return resolution;
        }
        if (catalog.catalog == ProviderCatalogState::Failed ||
            catalog.catalog == ProviderCatalogState::EndpointUnavailable) {
            resolution.error = ModelBindingError::ProviderUnavailable;
            resolution.reason = catalog.safeDetail;
            return resolution;
        }
        if (catalog.catalog == ProviderCatalogState::Empty ||
            (catalog.catalog == ProviderCatalogState::Available &&
             !catalog.modelIds.contains(model))) {
            resolution.error = ModelBindingError::ModelNotFound;
            resolution.reason =
                catalog.catalog == ProviderCatalogState::Empty
                    ? catalog.safeDetail
                    : QStringLiteral("Selected model is unavailable in the provider catalog.");
            return resolution;
        }
    }

    resolution.provider = constructProvider(resolution.binding);
    if (!resolution.provider) {
        resolution.error = ModelBindingError::ProviderUnavailable;
        resolution.reason = QStringLiteral("Provider '%1' could not be constructed.").arg(provider);
    }
    return resolution;
}

std::shared_ptr<IChatProvider> ModelService::constructProvider(const ModelBinding& binding) const {
    const auto factory = providerFactories_.value(binding.providerId);
    auto provider = factory ? factory(binding) : nullptr;
    return provider ? std::make_shared<HealthTrackedProvider>(
                          binding.providerId, std::move(provider), providerHealthRegistry_)
                    : nullptr;
}

ProviderHealth ModelService::providerHealth(const QString& providerId) const {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    return providerHealthRegistry_->entries.value(normalizedProviderId(providerId)).health;
}

ProviderCompletenessStatus ModelService::providerStatus(const QString& providerId,
                                                        const QString& modelId) const {
    ProviderCompletenessStatus status;
    status.providerId = normalizedProviderId(providerId);
    status.health = providerHealth(status.providerId);
    const auto selected = modelId.trimmed().isEmpty() && selection_.providerId == status.providerId
                              ? selection_.modelId
                              : modelId.trimmed();
    status.capabilities = capabilities(status.providerId, selected);
    if (!isKnownProvider(status.providerId)) {
        status.catalog = ProviderCatalogState::Failed;
        status.errorCategory = ChatProviderErrorCategory::ProviderUnavailable;
        status.safeDetail = QStringLiteral("Provider is not registered.");
        return status;
    }
    if (status.providerId == QLatin1String("ollama")) {
        const auto discovery = ollamaDiscovery();
        status.errorCategory = discovery.errorCategory;
        status.safeDetail = discovery.safeDetail;
        if (discovery.succeeded()) {
            for (const auto& model : discoveredOllamaModels())
                status.modelIds.append(model.name);
            status.catalog = status.modelIds.isEmpty() ? ProviderCatalogState::Empty
                                                       : ProviderCatalogState::Available;
        } else if (discovery.lifecycle == ChatRequestLifecycle::Pending ||
                   discovery.lifecycle == ChatRequestLifecycle::Running) {
            status.catalog = ProviderCatalogState::Pending;
        } else {
            for (const auto& model : discoveredOllamaModels())
                status.modelIds.append(model.name);
            status.catalog =
                !status.modelIds.isEmpty() ? ProviderCatalogState::Stale
                : discovery.errorCategory == ChatProviderErrorCategory::AuthenticationRequired
                    ? ProviderCatalogState::AuthenticationRequired
                : discovery.errorCategory == ChatProviderErrorCategory::ConnectionFailed
                    ? ProviderCatalogState::EndpointUnavailable
                    : ProviderCatalogState::Failed;
        }
        return status;
    }
    const ModelBinding binding{status.providerId, selected};
    const auto config = providerConfig(binding);
    if (config.isCloud() && config.apiKey.trimmed().isEmpty()) {
        status.catalog = ProviderCatalogState::AuthenticationRequired;
        status.errorCategory = ChatProviderErrorCategory::AuthenticationRequired;
        status.safeDetail = QStringLiteral("Provider API key is not configured.");
    } else if (!config.isAllowedEndpoint()) {
        status.catalog = ProviderCatalogState::EndpointUnavailable;
        status.errorCategory = ChatProviderErrorCategory::ProviderUnavailable;
        status.safeDetail = QStringLiteral("Provider endpoint is not configured or allowed.");
    } else {
        ProviderHealthRegistry::Catalog catalog;
        {
            std::lock_guard lock(providerHealthRegistry_->mutex);
            catalog = providerHealthRegistry_->catalogs.value(status.providerId);
        }
        if (catalog.observed) {
            status.errorCategory = catalog.outcome.category;
            for (const auto& model : catalog.models)
                status.modelIds.append(model.name);
            if (catalog.outcome.completed) {
                status.catalog = status.modelIds.isEmpty() ? ProviderCatalogState::Empty
                                                           : ProviderCatalogState::Available;
                status.safeDetail =
                    status.modelIds.isEmpty()
                        ? QStringLiteral("Provider returned an empty model catalog.")
                        : QStringLiteral("Provider model catalog is available.");
            } else {
                if (config.isCloud() && catalog.models.isEmpty() &&
                    catalog.outcome.httpStatus == 404) {
                    status.catalog = ProviderCatalogState::ConfiguredModelOnly;
                    if (status.modelIds.isEmpty() && !selected.isEmpty())
                        status.modelIds.append(selected);
                    status.safeDetail =
                        QStringLiteral("Provider model-list endpoint is unavailable; configured "
                                       "model is unverified.");
                    return status;
                }
                status.catalog =
                    !status.modelIds.isEmpty() ? ProviderCatalogState::Stale
                    : catalog.outcome.category == ChatProviderErrorCategory::AuthenticationRequired
                        ? ProviderCatalogState::AuthenticationRequired
                    : catalog.outcome.category == ChatProviderErrorCategory::ConnectionFailed
                        ? ProviderCatalogState::EndpointUnavailable
                        : ProviderCatalogState::Failed;
                status.safeDetail =
                    QStringLiteral("Provider model discovery failed (%1).")
                        .arg(chatProviderErrorCategoryName(catalog.outcome.category));
            }
        } else if (config.isCloud()) {
            status.catalog = ProviderCatalogState::ConfiguredModelOnly;
            if (!selected.isEmpty())
                status.modelIds.append(selected);
            status.safeDetail =
                QStringLiteral("Configured model; live provider catalog is unavailable.");
        } else {
            status.catalog = ProviderCatalogState::Unverified;
            status.safeDetail = QStringLiteral("Local endpoint catalog has not been observed yet.");
        }
    }
    return status;
}

ProviderCompletenessStatus ModelService::providerStatusSnapshot(const QString& providerId,
                                                                const QString& modelId) const {
    const auto id = normalizedProviderId(providerId);
    if (!isCloudProviderId(id))
        return providerStatus(id, modelId);
    ProviderCompletenessStatus status;
    status.providerId = id;
    status.health = providerHealth(id);
    status.capabilities = mergedCapabilities({}, providerCapabilities_.value(id),
                                             ModelMetadataSource::ProviderDefault);
    ProviderHealthRegistry::Catalog catalog;
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        catalog = providerHealthRegistry_->catalogs.value(id);
    }
    if (!catalog.observed) {
        status.safeDetail =
            QStringLiteral("Catalog unverified; credentials are not inspected by terminal status. "
                           "Select this provider and refresh discovery explicitly.");
        return status;
    }
    status.errorCategory = catalog.outcome.category;
    for (const auto& model : catalog.models)
        status.modelIds.append(model.name);
    if (catalog.outcome.completed) {
        status.catalog = status.modelIds.isEmpty() ? ProviderCatalogState::Empty
                                                   : ProviderCatalogState::Available;
        status.safeDetail = QStringLiteral("Observed provider model catalog.");
    } else {
        status.catalog =
            !status.modelIds.isEmpty() ? ProviderCatalogState::Stale
            : catalog.outcome.category == ChatProviderErrorCategory::AuthenticationRequired
                ? ProviderCatalogState::AuthenticationRequired
                : ProviderCatalogState::Failed;
        status.safeDetail = QStringLiteral("Observed discovery failure (%1).")
                                .arg(chatProviderErrorCategoryName(catalog.outcome.category));
    }
    return status;
}

QList<OllamaModelSummary> ModelService::providerDiscoveredModels(const QString& providerId) const {
    const auto id = normalizedProviderId(providerId);
    std::lock_guard lock(providerHealthRegistry_->mutex);
    if (id == QLatin1String("ollama"))
        return providerHealthRegistry_->ollamaModels;
    return providerHealthRegistry_->catalogs.value(id).models;
}

void ModelService::acceptProviderDiscovery(const QString& providerId,
                                           const QList<OllamaModelSummary>& models,
                                           const ProviderDiscoveryOutcome& outcome,
                                           quint64 sequence, bool nativeCatalog) {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    auto& catalog = providerHealthRegistry_->catalogs[normalizedProviderId(providerId)];
    if (sequence < catalog.sequence)
        return;
    if (catalog.nativeCatalog && !nativeCatalog) {
        catalog.sequence = sequence;
        return;
    }
    catalog.sequence = sequence;
    catalog.observed = true;
    catalog.outcome = outcome;
    if (outcome.completed && (!catalog.nativeCatalog || nativeCatalog)) {
        catalog.models = models;
        catalog.nativeCatalog = nativeCatalog;
    }
}

void ModelService::acceptOllamaDiscovery(const OllamaModelDiscoveryResult& result,
                                         quint64 sequence) {
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        if (sequence < providerHealthRegistry_->ollamaDiscoverySequence)
            return;
        providerHealthRegistry_->ollamaDiscoverySequence = sequence;
        providerHealthRegistry_->ollamaDiscovery = result;
        if (result.succeeded())
            providerHealthRegistry_->ollamaModels = result.models;
    }
    reportProviderDiscovery(QStringLiteral("ollama"), sequence, result.succeeded(),
                            result.errorCategory);
}

OllamaModelDiscoveryResult ModelService::ollamaDiscovery() const {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    return providerHealthRegistry_->ollamaDiscovery;
}

QList<OllamaModelSummary> ModelService::discoveredOllamaModels() const {
    std::lock_guard lock(providerHealthRegistry_->mutex);
    return providerHealthRegistry_->ollamaModels;
}

void ModelService::applyOllamaModelMutation(const QString& modelId, bool installed) {
    const auto name = modelId.trimmed();
    if (name.isEmpty())
        return;
    std::lock_guard lock(providerHealthRegistry_->mutex);
    auto& models = providerHealthRegistry_->ollamaModels;
    models.removeIf([&](const auto& model) { return model.name == name; });
    if (providerHealthRegistry_->ollamaDiscovery.succeeded()) {
        providerHealthRegistry_->ollamaDiscovery.models.removeIf(
            [&](const auto& model) { return model.name == name; });
    }
    if (installed) {
        OllamaModelSummary model;
        model.name = name;
        models.append(model);
        if (providerHealthRegistry_->ollamaDiscovery.succeeded())
            providerHealthRegistry_->ollamaDiscovery.models.append(model);
    }
}

quint64 ModelService::beginProviderHealthObservation() const {
    return ++providerHealthRegistry_->nextSequence;
}

void ModelService::reportProviderDiscovery(const QString& providerId, quint64 sequence,
                                           bool completed, ChatProviderErrorCategory category) {
    reportProviderRequest(providerId, sequence, completed, category, QStringLiteral("discovery"));
}

void ModelService::reportProviderRequest(const QString& providerId, quint64 sequence,
                                         bool completed, ChatProviderErrorCategory category,
                                         const QString& source) {
    bool changed = false;
    {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        auto& entry = providerHealthRegistry_->entries[normalizedProviderId(providerId)];
        if (sequence < entry.sequence)
            return;
        const auto previous = entry.health;
        entry.sequence = sequence;
        entry.source = source;
        if (completed)
            entry.health = ProviderHealth::Available;
        else if (category == ChatProviderErrorCategory::RateLimited ||
                 category == ChatProviderErrorCategory::ConnectionFailed ||
                 category == ChatProviderErrorCategory::Timeout ||
                 category == ChatProviderErrorCategory::ProviderUnavailable)
            entry.health = ProviderHealth::Degraded;
        else if (category == ChatProviderErrorCategory::AuthenticationRequired ||
                 category == ChatProviderErrorCategory::RequestRejected ||
                 category == ChatProviderErrorCategory::MalformedResponse)
            entry.health = ProviderHealth::Unavailable;
        changed = previous != entry.health;
    }
    if (changed)
        emit providerHealthChanged();
}

LMStudioConfig ModelService::providerConfig(const ModelBinding& binding) const {
    AppSettings* liveSettings = settings_;
    std::unique_ptr<AppSettings> fallbackSettings;
    if (!liveSettings) {
        const QString settingsPath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                .filePath(QStringLiteral("settings.json"));
        fallbackSettings =
            std::make_unique<AppSettings>(std::make_unique<JsonSettingsStore>(settingsPath));
        fallbackSettings->disablePersistence();
        liveSettings = fallbackSettings.get();
    }

    LMStudioConfig config;
    const QString cloudProv = liveSettings->selectedCloudProvider().toLower().trimmed();

    const auto* descriptor =
        ProviderRegistry::builtins().endpointDescriptor(binding.providerId, cloudProv);
    config.endpoint = descriptor && !descriptor->baseEndpoint.isEmpty()
                          ? QUrl(descriptor->baseEndpoint)
                          : QUrl(lmStudioEndpoint_);
    if (descriptor) {
        switch (descriptor->endpoint) {
        case ProviderEndpoint::Anthropic:
            config.apiKey = liveSettings->claudeApiKey();
            break;
        case ProviderEndpoint::Gemini:
            config.apiKey = liveSettings->geminiApiKey();
            break;
        case ProviderEndpoint::DeepSeek:
            config.apiKey = liveSettings->deepseekApiKey();
            break;
        case ProviderEndpoint::Groq:
            config.apiKey = liveSettings->groqApiKey();
            break;
        case ProviderEndpoint::Mistral:
            config.apiKey = liveSettings->mistralApiKey();
            break;
        case ProviderEndpoint::OpenAi:
            config.apiKey = liveSettings->openAiApiKey();
            break;
        case ProviderEndpoint::LlamaCpp:
            config.endpoint = QUrl(llamaCppEndpoint_);
            break;
        default:
            break;
        }
    }
    config.networkFactory = providerNetworkFactory_;
    return config;
}

void ModelService::setOllamaEndpoint(const QString& endpoint) {
    if (ollamaEndpoint_ != endpoint) {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        providerHealthRegistry_->ollamaModels.clear();
        providerHealthRegistry_->ollamaDiscovery = {};
        providerHealthRegistry_->ollamaDiscovery.lifecycle = ChatRequestLifecycle::Pending;
        providerHealthRegistry_->ollamaDiscovery.errorCategory = ChatProviderErrorCategory::None;
        providerHealthRegistry_->ollamaDiscovery.safeDetail =
            QStringLiteral("Ollama model discovery pending.");
        providerHealthRegistry_->entries.remove(QStringLiteral("ollama"));
    }
    ollamaEndpoint_ = endpoint;
}

void ModelService::setLmStudioEndpoint(const QString& endpoint) {
    if (lmStudioEndpoint_ != endpoint) {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        providerHealthRegistry_->catalogs.remove(QStringLiteral("lm-studio"));
    }
    lmStudioEndpoint_ = endpoint;
}

void ModelService::setLlamaCppEndpoint(const QString& endpoint) {
    if (llamaCppEndpoint_ != endpoint) {
        std::lock_guard lock(providerHealthRegistry_->mutex);
        providerHealthRegistry_->catalogs.remove(QStringLiteral("llama-cpp-server"));
    }
    llamaCppEndpoint_ = endpoint;
}

void ModelService::setLocalInferenceTimeoutMs(int timeoutMs) {
    localInferenceTimeoutMs_ = timeoutMs;
}

void ModelService::setProviderNetworkFactoryForTesting(
    std::shared_ptr<ILocalInferenceNetworkFactory> networkFactory) {
    providerNetworkFactory_ = std::move(networkFactory);
}

} // namespace sentinel::core
