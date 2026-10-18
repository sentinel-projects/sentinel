// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/model/ModelRouting.h"
#include "sentinel/core/provider/SelectedEndpointChatProvider.h"
#include "sentinel/core/runtime/OllamaRuntime.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace sentinel::core {

class AppSettings;
class IModelRouter;
class ILocalInferenceNetworkFactory;
struct LMStudioConfig;
struct ProviderHealthRegistry;

enum class ProviderHealth { Unknown, Available, Degraded, Unavailable };
QString providerHealthName(ProviderHealth health);

enum class ProviderCatalogState {
    Pending,
    Available,
    Empty,
    Failed,
    AuthenticationRequired,
    EndpointUnavailable,
    Stale,
    ConfiguredModelOnly,
    Unverified,
};
QString providerCatalogStateName(ProviderCatalogState state);

struct ProviderCompletenessStatus {
    QString providerId;
    ProviderCatalogState catalog = ProviderCatalogState::Unverified;
    ProviderHealth health = ProviderHealth::Unknown;
    ChatProviderErrorCategory errorCategory = ChatProviderErrorCategory::None;
    ModelCapabilities capabilities;
    QStringList modelIds;
    QString safeDetail;
};

struct CurrentModelMetadata {
    QString providerId;
    QString modelId;
    ProviderKind providerKind = ProviderKind::Local;
    ProviderCatalogState catalog = ProviderCatalogState::Unverified;
    ModelCapabilities capabilities;
    QString family;
    QString publisher;
    QString architecture;
};

// Authoritative provider/model selection for new interactive requests.
struct ModelSelection {
    QString providerId;
    QString modelId;

    bool isValid() const {
        return !providerId.isEmpty() && !modelId.isEmpty();
    }
    bool operator==(const ModelSelection& other) const {
        return providerId == other.providerId && modelId == other.modelId;
    }
    bool operator!=(const ModelSelection& other) const {
        return !(*this == other);
    }
};

// Provider-neutral binding failures. Presentations format these into strings.
enum class ModelBindingError {
    None,
    ProviderNotFound,
    ProviderUnavailable,
    ModelNotFound,
    ConfigurationInvalid,
    AuthenticationRequired,
};

QString modelBindingErrorName(ModelBindingError error);

// Immutable provider/model identity for one chat request, agent run, or
// controlled task, plus the shared provider handle resolved for it. Carries
// provenance (providerId, modelId, reason) and never credentials.
struct ModelBindingResolution {
    ModelBindingError error = ModelBindingError::None;
    ModelBinding binding;
    ModelSelection selection;
    std::shared_ptr<IChatProvider> provider;
    QString reason;

    bool ok() const {
        return error == ModelBindingError::None && provider != nullptr;
    }
};

QString modelBindingFailureSummary(const ModelBindingResolution& resolution);

// Lazy provider construction policy for one stable provider id.
using ModelProviderFactory = std::function<std::shared_ptr<IChatProvider>(const ModelBinding&)>;

// Application-lifetime owner of provider registration/construction, the single
// current ModelSelection, and run-level ModelBinding creation for Chat Mode,
// Agent Mode, and controlled tasks. It owns no agent, chat, tool, or
// presentation state. Access is main-thread; resolved providers leave the
// registry as shared_ptr leases so executions never hold registry locks.
class ModelService final : public QObject {
    Q_OBJECT
public:
    explicit ModelService(AppSettings* settings = nullptr, QObject* parent = nullptr);
    ~ModelService() override;

    // Resolution policy/mechanics stay behind IModelRouter; this service owns
    // application provider identity and selection state around it.
    void setModelRouter(IModelRouter* router);
    IModelRouter* modelRouter() const {
        return router_;
    }

    // One authoritative provider registry: providerId -> factory. Built-in
    // providers register at construction; composition roots may override.
    void registerProvider(const QString& providerId, ModelProviderFactory factory,
                          ModelCapabilities defaults = {});
    void setModelCapabilities(const QString& providerId, const QString& modelId,
                              ModelCapabilities capabilities);
    ModelCapabilities capabilities(const QString& providerId, const QString& modelId) const;
    CurrentModelMetadata currentModelMetadata(const QString& providerId,
                                              const QString& modelId) const;
    ModelCapabilities capabilityOverrides(const QString& providerId, const QString& modelId) const;
    void setCapabilityOverrides(const QString& providerId, const QString& modelId,
                                const ModelCapabilities& overrides);
    void clearCapabilityOverrides(const QString& providerId, const QString& modelId);
    bool isKnownProvider(const QString& providerId) const;
    QStringList knownProviderIds() const;

    // Single authoritative current selection, restored from and persisted to
    // the existing settings infrastructure when available.
    ModelSelection selectedModel() const;
    void setSelectedModel(const ModelSelection& selection);
    void setSelectedProviderId(const QString& providerId);
    void setSelectedModelId(const QString& modelId);

    // Provider-neutral binding for current or captured provider/model pairs.
    ModelBindingResolution resolve(const ModelSelection& selection);
    ModelBindingResolution resolve(const QString& providerId, const QString& modelId);

    // Endpoint/credential configuration used for provider construction.
    LMStudioConfig providerConfig(const ModelBinding& binding) const;
    std::shared_ptr<IChatProvider> constructProvider(const ModelBinding& binding) const;
    ProviderHealth providerHealth(const QString& providerId) const;
    ProviderCompletenessStatus providerStatus(const QString& providerId,
                                              const QString& modelId = {}) const;
    // Safe observed status; never reads cloud credentials or performs discovery.
    ProviderCompletenessStatus providerStatusSnapshot(const QString& providerId,
                                                      const QString& modelId = {}) const;
    QList<OllamaModelSummary> providerDiscoveredModels(const QString& providerId) const;
    void acceptProviderDiscovery(const QString& providerId, const QList<OllamaModelSummary>& models,
                                 const ProviderDiscoveryOutcome& outcome, quint64 sequence,
                                 bool nativeCatalog = false);
    void acceptOllamaDiscovery(const OllamaModelDiscoveryResult& result, quint64 sequence);
    OllamaModelDiscoveryResult ollamaDiscovery() const;
    QList<OllamaModelSummary> discoveredOllamaModels() const;
    void applyOllamaModelMutation(const QString& modelId, bool installed);
    quint64 beginProviderHealthObservation() const;
    void reportProviderDiscovery(const QString& providerId, quint64 sequence, bool completed,
                                 ChatProviderErrorCategory category);
    void reportProviderRequest(const QString& providerId, quint64 sequence, bool completed,
                               ChatProviderErrorCategory category, const QString& source);

    // Construction inputs pushed by the application composition. Endpoint and
    // timeout settings are applied here so provider construction never reaches
    // back into the controller.
    void setOllamaEndpoint(const QString& endpoint);
    void setLmStudioEndpoint(const QString& endpoint);
    void setLlamaCppEndpoint(const QString& endpoint);
    void setLocalInferenceTimeoutMs(int timeoutMs);

    // Safely constrained transport seam for deterministic provider-contract
    // fixtures. Empty in production, so constructed provider configurations keep
    // the existing authenticated HTTPS endpoints and network policy. A fixture
    // may install an in-memory factory to observe the exact intended URL,
    // headers and serialized body without issuing a network request.
    void setProviderNetworkFactoryForTesting(
        std::shared_ptr<ILocalInferenceNetworkFactory> networkFactory);

signals:
    void selectedModelChanged();
    void providerRegistryChanged();
    void providerHealthChanged();
    void modelCapabilitiesChanged();

private:
    AppSettings* settings_ = nullptr;
    IModelRouter* router_ = nullptr;
    ModelSelection selection_;
    QHash<QString, ModelProviderFactory> providerFactories_;
    QHash<QString, ModelCapabilities> providerCapabilities_;
    QHash<QString, QHash<QString, ModelCapabilities>> modelCapabilities_;
    QString ollamaEndpoint_;
    QString lmStudioEndpoint_;
    QString llamaCppEndpoint_;
    int localInferenceTimeoutMs_ = 0;
    std::shared_ptr<ILocalInferenceNetworkFactory> providerNetworkFactory_;
    std::shared_ptr<ProviderHealthRegistry> providerHealthRegistry_;
};

} // namespace sentinel::core
