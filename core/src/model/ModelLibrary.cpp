// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/model/ModelLibrary.h"
#include "sentinel/core/model/ModelStorageManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QtEndian>
#include <algorithm>

namespace sentinel::core {
namespace {

bool isCloud(const QString& id) {
    return id == QLatin1String("cloud-api") || id == QLatin1String("openai-compatible") ||
           id == QLatin1String("openai") || id == QLatin1String("claude") ||
           id == QLatin1String("gemini") || id == QLatin1String("deepseek") ||
           id == QLatin1String("groq") || id == QLatin1String("mistral");
}

QString label(const QString& id) {
    if (id == QLatin1String("lm-studio")) return QStringLiteral("LM Studio");
    if (id == QLatin1String("llama-cpp-server")) return QStringLiteral("llama.cpp server");
    if (id == QLatin1String("openai-compatible-local"))
        return QStringLiteral("OpenAI-compatible local");
    if (id == QLatin1String("openai-compatible"))
        return QStringLiteral("OpenAI-compatible");
    if (id == QLatin1String("cloud-api")) return QStringLiteral("Cloud API");
    if (id == QLatin1String("ollama")) return QStringLiteral("Ollama");
    if (id == QLatin1String("claude")) return QStringLiteral("Claude");
    if (id == QLatin1String("gemini")) return QStringLiteral("Gemini");
    return id.left(1).toUpper() + id.mid(1);
}

ModelLibraryCatalogState catalogState(ProviderCatalogState state, bool ollama) {
    switch (state) {
    case ProviderCatalogState::Available:
        return ollama ? ModelLibraryCatalogState::LocalOnly : ModelLibraryCatalogState::Current;
    case ProviderCatalogState::Stale: return ModelLibraryCatalogState::StaleCached;
    case ProviderCatalogState::Empty: return ModelLibraryCatalogState::Empty;
    case ProviderCatalogState::ConfiguredModelOnly:
        return ModelLibraryCatalogState::ConfiguredOnly;
    default: return ModelLibraryCatalogState::Unavailable;
    }
}

ModelInstallationStrategy strategy(const QString& providerId) {
    if (isCloud(providerId)) return ModelInstallationStrategy::CloudNoInstall;
    if (providerId == QLatin1String("ollama")) return ModelInstallationStrategy::RuntimeManaged;
    return ModelInstallationStrategy::ExternalApplicationManaged;
}

bool matchesSupport(CapabilitySupport support, const std::optional<bool>& requested) {
    return !requested || (support != CapabilitySupport::Unknown &&
                          (support == CapabilitySupport::Supported) == *requested);
}

QString encoded(const QString& value) {
    return QString::fromLatin1(QUrl::toPercentEncoding(value));
}

QString ggufQuantization(const QString& fileName) {
    static const QRegularExpression pattern(
        QStringLiteral("(?:^|[._-])(Q(?:[2-8]_[A-Z0-9_]+|[48]_0)|F16|F32)\\.gguf$"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = pattern.match(fileName);
    return match.hasMatch() ? match.captured(1).toUpper() : QString{};
}

} // namespace

bool validGgufHeader(const QByteArray& prefix) {
    if (prefix.size() < 24 || !prefix.startsWith(QByteArrayLiteral("GGUF"))) return false;
    const auto* bytes = reinterpret_cast<const uchar*>(prefix.constData());
    const auto version = qFromLittleEndian<quint32>(bytes + 4);
    const auto tensorCount = qFromLittleEndian<quint64>(bytes + 8);
    return (version == 2 || version == 3) && tensorCount > 0;
}

QString LMStudioNativeCatalogAdapter::sourceId() const {
    return QStringLiteral("lm-studio-native");
}

QString LMStudioNativeCatalogAdapter::providerId() const {
    return QStringLiteral("lm-studio");
}

QList<ModelLibraryEntry> LMStudioNativeCatalogAdapter::entries() const { return entries_; }

QList<OllamaModelSummary> LMStudioNativeCatalogAdapter::discoveredModels() const {
    return discovered_;
}

bool LMStudioNativeCatalogAdapter::hasSnapshot() const { return hasSnapshot_; }

void LMStudioNativeCatalogAdapter::markStale() {
    for (auto& entry : entries_) {
        entry.catalog = ModelLibraryCatalogState::StaleCached;
        entry.availability = ModelLibraryAvailability::Unknown;
    }
}

void LMStudioNativeCatalogAdapter::clear() {
    entries_.clear();
    discovered_.clear();
    hasSnapshot_ = false;
}

bool LMStudioNativeCatalogAdapter::acceptResponse(const QByteArray& response) {
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(response, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        !document.object().value(QStringLiteral("models")).isArray()) return false;
    QList<ModelLibraryEntry> nextEntries;
    QList<OllamaModelSummary> nextDiscovered;
    for (const auto& value : document.object().value(QStringLiteral("models")).toArray()) {
        if (!value.isObject()) return false;
        const auto item = value.toObject();
        const auto key = item.value(QStringLiteral("key")).toString().trimmed();
        if (key.isEmpty()) return false;
        ModelLibraryEntry entry;
        entry.provider = {providerId(), QStringLiteral("LM Studio"), ProviderKind::Local};
        entry.runtime = {providerId(), QStringLiteral("LM Studio")};
        entry.source = {sourceId(), QStringLiteral("LM Studio native catalog"),
                        QStringLiteral("runtime-reported"), {}};
        entry.nativeModelId = key;
        entry.displayName = item.value(QStringLiteral("display_name")).toString().trimmed();
        if (entry.displayName.isEmpty()) entry.displayName = key;
        entry.publisher = item.value(QStringLiteral("publisher")).toString();
        entry.architecture = item.value(QStringLiteral("architecture")).toString();
        entry.format = item.value(QStringLiteral("format")).toString();
        entry.quantization = item.value(QStringLiteral("quantization")).toObject()
                                 .value(QStringLiteral("name")).toString();
        const auto size = item.value(QStringLiteral("size_bytes")).toVariant().toLongLong();
        if (size > 0) entry.sizeBytes = entry.diskBytes = size;
        entry.artifactId = key;
        entry.local = true;
        entry.installed = ModelLibraryInstalledState::Installed;
        entry.availability = ModelLibraryAvailability::Available;
        entry.installation = ModelInstallationStrategy::ExternalApplicationManaged;
        entry.catalog = ModelLibraryCatalogState::LocalOnly;
        entry.sourceProvenance = QStringLiteral("LM Studio /api/v1/models");
        entry.externalManagerUrl = QStringLiteral("lmstudio://");
        entry.externalManagerAvailable = true;
        nextEntries.append(entry);

        OllamaModelSummary discovered;
        discovered.name = key;
        discovered.sizeBytes = size;
        discovered.publisher = entry.publisher;
        discovered.architecture = entry.architecture;
        const auto maxContext = item.value(QStringLiteral("max_context_length")).toInt();
        if (maxContext > 0) {
            discovered.capabilities.contextWindow = maxContext;
            // A published maximum is a property of the model weights.
            discovered.capabilities.contextCapacity = ContextCapacityKind::ModelMaximum;
            discovered.capabilities.provenance.contextCapacity =
                ModelMetadataSource::ProviderCatalog;
        }
        // A published maximum is not the context of a loaded inference instance.
        // Use the smallest reported loaded window because requests use the model key.
        bool sawLoadedWindow = false;
        for (const auto& instance : item.value(QStringLiteral("loaded_instances")).toArray()) {
            const auto loaded = instance.toObject()
                                    .value(QStringLiteral("config"))
                                    .toObject()
                                    .value(QStringLiteral("context_length"))
                                    .toInt();
            if (loaded > 0) {
                discovered.capabilities.contextWindow =
                    qMin(discovered.capabilities.contextWindow.value_or(loaded), loaded);
                sawLoadedWindow = true;
            }
        }
        if (sawLoadedWindow) {
            // The server reported the capacity its loaded instances run with, so
            // this value is authoritative for token admission.
            discovered.capabilities.contextCapacity = ContextCapacityKind::ServerConfigured;
            discovered.capabilities.provenance.contextWindow = ModelMetadataSource::RuntimeReported;
            discovered.capabilities.provenance.contextCapacity =
                ModelMetadataSource::RuntimeReported;
        }
        const auto capabilities = item.value(QStringLiteral("capabilities")).toObject();
        if (capabilities.value(QStringLiteral("vision")).isBool())
            discovered.capabilities.visionInput =
                capabilities.value(QStringLiteral("vision")).toBool()
                    ? CapabilitySupport::Supported : CapabilitySupport::Unsupported;
        if (capabilities.value(QStringLiteral("trained_for_tool_use")).isBool())
            discovered.capabilities.nativeToolCalling =
                capabilities.value(QStringLiteral("trained_for_tool_use")).toBool()
                    ? CapabilitySupport::Supported : CapabilitySupport::Unsupported;
        nextDiscovered.append(discovered);
    }
    entries_ = std::move(nextEntries);
    discovered_ = std::move(nextDiscovered);
    hasSnapshot_ = true;
    return true;
}

ModelLibraryService::ModelLibraryService(const ModelService& models, QString registrationsPath)
    : models_(models), registrationsPath_(std::move(registrationsPath)) {
    if (registrationsPath_.isEmpty()) {
        registrationsPath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
            .filePath(QStringLiteral("model-library-local.json"));
    }
    QFile file(registrationsPath_);
    if (!file.open(QIODevice::ReadOnly)) return;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isArray()) return;
    for (const auto& value : document.array()) {
        if (!value.isObject()) continue;
        const auto item = value.toObject();
        const auto path = item.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            registrations_.append({path, item.value(QStringLiteral("runtimeModelId")).toString(),
                item.value(QStringLiteral("repositoryId")).toString(),
                item.value(QStringLiteral("artifactFilename")).toString(),
                item.value(QStringLiteral("revision")).toString(),
                item.value(QStringLiteral("artifactHash")).toString()});
    }
}

bool ModelLibraryService::saveRegistrations() const {
    const QFileInfo target(registrationsPath_);
    if (!QDir().mkpath(target.absolutePath())) return false;
    QJsonArray data;
    for (const auto& registration : registrations_)
        data.append(QJsonObject{{QStringLiteral("path"), registration.filePath},
                                {QStringLiteral("runtimeModelId"), registration.runtimeModelId},
                                {QStringLiteral("repositoryId"), registration.repositoryId},
                                {QStringLiteral("artifactFilename"), registration.artifactFilename},
                                {QStringLiteral("revision"), registration.revision},
                                {QStringLiteral("artifactHash"), registration.artifactHash}});
    QSaveFile file(registrationsPath_);
    if (!file.open(QIODevice::WriteOnly)) return false;
    const auto payload = QJsonDocument(data).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) return false;
    return file.commit();
}

bool ModelLibraryService::registerLocalGguf(const QString& filePath,
                                             const QString& runtimeModelId,
                                             bool* storageFailure,
                                             const ModelLibraryEntry* origin) {
    if (storageFailure) *storageFailure = false;
    const QFileInfo info(filePath);
    if (!info.isFile() || !info.isReadable() || info.size() < 24 ||
        info.suffix().compare(QStringLiteral("gguf"), Qt::CaseInsensitive) != 0) return false;
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly) || !validGgufHeader(file.read(24))) return false;
    const auto path = info.canonicalFilePath();
    const auto previous = registrations_;
    auto existing = std::find_if(registrations_.begin(), registrations_.end(),
                                 [&](const auto& item) { return item.filePath == path; });
    LocalGgufRegistration registration{path, runtimeModelId.trimmed()};
    if (origin) {
        registration.repositoryId = origin->repositoryId;
        registration.artifactFilename = origin->artifactFilename;
        registration.revision = origin->revision;
        registration.artifactHash = origin->artifactHash;
    }
    if (existing == registrations_.end()) registrations_.append(registration);
    else *existing = registration;
    if (saveRegistrations()) return true;
    registrations_ = previous;
    if (storageFailure) *storageFailure = true;
    return false;
}

bool ModelLibraryService::removeLocalGgufRegistration(const QString& id,
                                                       bool* storageFailure) {
    if (storageFailure) *storageFailure = false;
    const auto previous = registrations_;
    registrations_.removeIf([&](const auto& item) {
        return entryId(QStringLiteral("llama-cpp-server"), QStringLiteral("llama-cpp-server"),
                       QStringLiteral("local-file"), item.filePath) == id;
    });
    if (registrations_.size() == previous.size()) return false;
    if (saveRegistrations()) return true;
    registrations_ = previous;
    if (storageFailure) *storageFailure = true;
    return false;
}

std::optional<ModelLibraryEntry>
ModelLibraryService::localGgufCandidate(const QString& filePath) const {
    const QFileInfo info(filePath);
    if (!info.isFile() || !info.isReadable() || info.size() < 24 ||
        info.suffix().compare(QStringLiteral("gguf"), Qt::CaseInsensitive) != 0) return std::nullopt;
    QFile file(info.canonicalFilePath());
    if (!file.open(QIODevice::ReadOnly) || !validGgufHeader(file.read(24)))
        return std::nullopt;
    const auto path = info.canonicalFilePath();
    for (const auto& registration : registrations_)
        if (registration.filePath == path) return std::nullopt;
    ModelLibraryEntry entry;
    entry.provider = {QStringLiteral("llama-cpp-server"), QStringLiteral("llama.cpp server"),
                      ProviderKind::Local};
    entry.runtime = {entry.provider.id, entry.provider.displayName};
    entry.source = {QStringLiteral("local-file"), QStringLiteral("Local GGUF"),
                    QStringLiteral("user-selected"), {}};
    entry.id = entryId(entry.provider.id, entry.runtime.id, entry.source.id, path);
    entry.displayName = info.fileName();
    entry.artifactId = entry.localFile = path;
    entry.format = QStringLiteral("GGUF");
    entry.quantization = ggufQuantization(info.fileName());
    entry.sizeBytes = entry.diskBytes = info.size();
    entry.installed = ModelLibraryInstalledState::NotInstalled;
    entry.installation = ModelInstallationStrategy::ImportLocal;
    entry.catalog = ModelLibraryCatalogState::LocalOnly;
    entry.sourceProvenance = QStringLiteral("user-selected local file");
    entry.actions = availableActions(entry);
    return entry;
}

void ModelLibraryService::addSourceAdapter(const IModelLibrarySourceAdapter* adapter) {
    if (adapter && !adapters_.contains(adapter)) adapters_.append(adapter);
}

QString ModelLibraryService::entryId(const QString& providerId, const QString& runtimeId,
                                     const QString& sourceId, const QString& nativeModelId) {
    return QStringLiteral("model-library/%1/%2/%3/%4")
        .arg(encoded(providerId.trimmed().toLower()), encoded(runtimeId.trimmed().toLower()),
             encoded(sourceId.trimmed().toLower()), encoded(nativeModelId.trimmed()));
}

QList<ModelLibraryProviderState> ModelLibraryService::providerStates() const {
    QList<ModelLibraryProviderState> states;
    for (const auto& id : models_.knownProviderIds()) {
        const auto status = models_.providerStatusSnapshot(id);
        const auto cloud = isCloud(id);
        states.append({{id, label(id), cloud ? ProviderKind::Cloud : ProviderKind::Local},
                       {cloud ? QStringLiteral("cloud") : id,
                        cloud ? QStringLiteral("Cloud") : label(id)},
                       catalogState(status.catalog, id == QLatin1String("ollama")),
                       status.safeDetail});
    }
    return states;
}

QList<ModelLibraryAction> ModelLibraryService::availableActions(const ModelLibraryEntry& entry) {
    QList<ModelLibraryAction> actions;
    if (entry.source.id == QLatin1String("hugging-face")) {
        if (!entry.artifactFilename.isEmpty() && !entry.revision.isEmpty() &&
            (entry.format == "GGUF" || entry.format == "safetensors" || entry.artifactFilename.endsWith(".bin") || entry.artifactFilename.endsWith(".onnx"))) {
            if (entry.installed != ModelLibraryInstalledState::Installed)
                actions.append(ModelLibraryAction::Download);
            if (entry.format == "GGUF" && entry.shardGroup.isEmpty() && !entry.registeredLocalFile)
                actions.append(ModelLibraryAction::DownloadAndRegister);
        }
        if (entry.managedStorage && !entry.registeredLocalFile)
            actions.append(ModelLibraryAction::Remove);
        if (!entry.localFile.isEmpty()) actions.append(ModelLibraryAction::RevealLocalFile);
        if (!entry.source.url.isEmpty()) actions.append(ModelLibraryAction::ViewSource);
        return actions;
    }
    if (!entry.nativeModelId.isEmpty() &&
        (entry.availability == ModelLibraryAvailability::Available ||
         entry.catalog == ModelLibraryCatalogState::ConfiguredOnly))
        actions.append(ModelLibraryAction::Select);
    if (entry.installed != ModelLibraryInstalledState::Installed) {
        if (entry.installation == ModelInstallationStrategy::SentinelManaged)
            actions.append(ModelLibraryAction::Install);
        if (entry.installation == ModelInstallationStrategy::RuntimeManaged)
            actions.append(ModelLibraryAction::Pull);
        if (entry.installation == ModelInstallationStrategy::ImportLocal &&
            !entry.registeredLocalFile)
            actions.append(ModelLibraryAction::Import);
    }
    if (entry.installed == ModelLibraryInstalledState::Installed &&
        (entry.installation == ModelInstallationStrategy::SentinelManaged ||
         entry.installation == ModelInstallationStrategy::RuntimeManaged))
        actions.append(ModelLibraryAction::Remove);
    if (entry.registeredLocalFile)
        actions.append(ModelLibraryAction::RemoveRegistration);
    if (entry.installation == ModelInstallationStrategy::ExternalApplicationManaged &&
        entry.externalManagerAvailable && !entry.externalManagerUrl.isEmpty())
        actions.append(ModelLibraryAction::OpenExternalManager);
    if (!entry.localFile.isEmpty() && QFileInfo(entry.localFile).isFile())
        actions.append(ModelLibraryAction::RevealLocalFile);
    if (!entry.source.url.isEmpty()) actions.append(ModelLibraryAction::ViewSource);
    return actions;
}

QList<ModelLibraryEntry> ModelLibraryService::entries(const IModelLibrarySourceAdapter* excludedSource) const {
    QList<ModelLibraryEntry> result;
    QSet<QString> identities;
    const auto selection = models_.selectedModel();
    for (const auto& state : providerStates()) {
        const auto status = models_.providerStatusSnapshot(state.provider.id);
        const auto discovered = models_.providerDiscoveredModels(state.provider.id);
        QHash<QString, OllamaModelSummary> details;
        for (const auto& model : discovered) details.insert(model.name, model);
        auto names = status.modelIds;
        if (state.provider.id == selection.providerId && !selection.modelId.isEmpty() &&
            (state.catalog == ModelLibraryCatalogState::ConfiguredOnly ||
             (state.catalog == ModelLibraryCatalogState::Unavailable &&
              status.catalog == ProviderCatalogState::Unverified)) &&
            !names.contains(selection.modelId)) names.append(selection.modelId);
        for (const auto& name : names) {
            if (name.trimmed().isEmpty()) continue;
            ModelLibraryEntry entry;
            entry.provider = state.provider;
            entry.runtime = state.runtime;
            entry.source = {state.provider.id + QStringLiteral("-discovery"),
                            state.provider.displayName,
                            details.contains(name) ? QStringLiteral("runtime-reported")
                                                   : QStringLiteral("provider-catalog"), {}};
            entry.nativeModelId = name;
            entry.displayName = name;
            entry.id = entryId(entry.provider.id, entry.runtime.id, entry.source.id, name);
            entry.local = state.provider.kind == ProviderKind::Local;
            entry.catalog = status.catalog == ProviderCatalogState::Unverified
                ? ModelLibraryCatalogState::ConfiguredOnly : state.catalog;
            entry.catalogDetail = state.detail;
            entry.sourceProvenance = entry.source.provenance;
            entry.installation = strategy(state.provider.id);
            entry.availability = state.catalog == ModelLibraryCatalogState::StaleCached ||
                                 status.catalog == ProviderCatalogState::Unverified
                ? ModelLibraryAvailability::Unknown
                : ModelLibraryAvailability::Available;
            entry.installed = state.provider.id == QLatin1String("ollama") &&
                    (state.catalog == ModelLibraryCatalogState::Current ||
                     state.catalog == ModelLibraryCatalogState::LocalOnly ||
                     state.catalog == ModelLibraryCatalogState::StaleCached)
                ? ModelLibraryInstalledState::Installed : ModelLibraryInstalledState::Unknown;
            const auto metadata = models_.currentModelMetadata(state.provider.id, name);
            entry.capabilities = metadata.capabilities;
            entry.family = metadata.family;
            entry.publisher = metadata.publisher;
            entry.architecture = metadata.architecture;
            if (details.contains(name) && details.value(name).sizeBytes > 0 && entry.local)
                entry.diskBytes = details.value(name).sizeBytes;
            if (state.provider.id == QLatin1String("lm-studio")) {
                entry.externalManagerUrl = QStringLiteral("lmstudio://");
                entry.externalManagerAvailable = true;
            }
            entry.actions = availableActions(entry);
            if (!identities.contains(entry.id)) {
                result.append(entry);
                identities.insert(entry.id);
            }
        }
    }
    for (const auto* adapter : adapters_) {
        if (adapter == excludedSource) continue;
        const bool externalSource = adapter->providerId().isEmpty();
        if (!externalSource && !models_.isKnownProvider(adapter->providerId())) continue;
        for (auto entry : adapter->entries()) {
            if (entry.nativeModelId.trimmed().isEmpty() && entry.artifactId.isEmpty()) continue;
            if (!externalSource) entry.provider.id = adapter->providerId();
            entry.source.id = adapter->sourceId();
            if (entry.runtime.id.isEmpty() && !externalSource) entry.runtime.id = entry.provider.id;
            entry.id = entryId(entry.provider.id, entry.runtime.id, entry.source.id,
                               externalSource ? entry.artifactId : entry.nativeModelId);
            if (!externalSource) {
                const auto metadata = models_.currentModelMetadata(entry.provider.id,
                                                                   entry.nativeModelId);
                entry.capabilities = metadata.capabilities;
                if (entry.family.isEmpty()) entry.family = metadata.family;
                if (entry.publisher.isEmpty()) entry.publisher = metadata.publisher;
                if (entry.architecture.isEmpty()) entry.architecture = metadata.architecture;
            }
            if (externalSource && !entry.localFile.isEmpty()) {
                for (const auto& registration : registrations_) {
                    if (registration.filePath == entry.localFile) {
                        entry.registeredLocalFile = true;
                        break;
                    }
                }
            }
            entry.actions = availableActions(entry);
            if (adapter->providerId() == QLatin1String("lm-studio")) {
                auto existing = std::find_if(result.begin(), result.end(), [&](const auto& current) {
                    return current.provider.id == entry.provider.id &&
                           current.runtime.id == entry.runtime.id &&
                           current.nativeModelId == entry.nativeModelId &&
                           current.source.id == QLatin1String("lm-studio-discovery");
                });
                if (existing != result.end()) {
                    identities.remove(existing->id);
                    *existing = entry;
                    identities.insert(entry.id);
                    continue;
                }
            }
            if (!identities.contains(entry.id)) {
                result.append(entry);
                identities.insert(entry.id);
            }
        }
    }
    for (const auto& registration : registrations_) {
        const QFileInfo file(registration.filePath);
        ModelLibraryEntry entry;
        entry.provider = {QStringLiteral("llama-cpp-server"), QStringLiteral("llama.cpp server"),
                          ProviderKind::Local};
        entry.runtime = {entry.provider.id, entry.provider.displayName};
        entry.source = {QStringLiteral("local-file"), QStringLiteral("Local GGUF"),
                        QStringLiteral("user-registered"), {}};
        entry.id = entryId(entry.provider.id, entry.runtime.id, entry.source.id,
                           registration.filePath);
        entry.displayName = file.fileName();
        entry.nativeModelId = registration.runtimeModelId;
        entry.artifactId = registration.filePath;
        entry.localFile = registration.filePath;
        entry.repositoryId = registration.repositoryId;
        entry.artifactFilename = registration.artifactFilename;
        entry.revision = registration.revision;
        entry.artifactHash = registration.artifactHash;
        entry.registeredLocalFile = true;
        entry.format = QStringLiteral("GGUF");
        entry.quantization = ggufQuantization(file.fileName());
        if (file.isFile()) entry.sizeBytes = entry.diskBytes = file.size();
        entry.installed = file.isFile() ? ModelLibraryInstalledState::Installed
                                        : ModelLibraryInstalledState::Unknown;
        const auto runtimeStatus = models_.providerStatusSnapshot(entry.provider.id);
        entry.availability = !entry.nativeModelId.isEmpty() &&
                runtimeStatus.modelIds.contains(entry.nativeModelId)
            ? ModelLibraryAvailability::Available : ModelLibraryAvailability::Unknown;
        entry.catalog = file.isFile() ? ModelLibraryCatalogState::LocalOnly
                                       : ModelLibraryCatalogState::Unavailable;
        entry.installation = ModelInstallationStrategy::ImportLocal;
        entry.sourceProvenance = QStringLiteral("user-registered local file");
        entry.actions = availableActions(entry);
        result.append(entry);
    }
    QSet<QString> managedPaths;
    if (storage_) {
        for (const auto& artifact : storage_->snapshot().artifacts)
            managedPaths.insert(artifact.path);
    }
    for (auto& entry : result) {
        entry.managedStorage = managedPaths.contains(entry.localFile);
        if (entry.diskBytes) {
            entry.fit.requiredDiskBytes = entry.diskBytes;
            entry.fit.diskEvidence = ModelResourceEvidence::Reported;
        } else if (entry.sizeBytes) {
            entry.fit.requiredDiskBytes = entry.sizeBytes;
            entry.fit.diskEvidence = ModelResourceEvidence::Reported;
        }
        const auto runtimeId = entry.runtime.id.isEmpty() &&
                               entry.format.compare(QStringLiteral("GGUF"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("llama-cpp-server") : entry.runtime.id;
        if (!runtimeId.isEmpty() && models_.isKnownProvider(runtimeId)) {
            const auto status = models_.providerStatusSnapshot(runtimeId);
            ModelRuntimeStatus runtime;
            runtime.runtimeId = runtimeId;
            if (status.catalog != ProviderCatalogState::Unverified)
                runtime.configured = true;
            if (status.health == ProviderHealth::Available) runtime.reachable = true;
            if (status.health == ProviderHealth::Unavailable) runtime.reachable = false;
            const auto reported = runtimeStatuses_.value(runtimeId);
            runtime.version = reported.version;
            runtime.activeModelIds = reported.activeModelIds;
            runtime.activeModelId = reported.activeModelId;
            runtime.accelerationBackend = reported.accelerationBackend;
            runtime.reportedActiveVramBytes = reported.reportedActiveVramBytes;
            entry.runtimeStatuses.append(runtime);
            if (entry.format.compare(QStringLiteral("GGUF"), Qt::CaseInsensitive) == 0) {
                if (!entry.shardGroup.isEmpty()) {
                    entry.fit.reasons << QStringLiteral("Sharded GGUF loading is not verified.");
                } else if (entry.localFile.isEmpty() &&
                           entry.source.id == QLatin1String("hugging-face")) {
                    entry.fit.reasons << QStringLiteral("Artifact is not stored locally.");
                } else if (!runtime.reachable.value_or(false)) {
                    entry.fit.reasons << QStringLiteral("llama.cpp runtime is not reachable.");
                } else {
                    entry.fit.state = ModelCompatibility::LikelyCompatible;
                    entry.fit.reasons << QStringLiteral("GGUF format and reachable llama.cpp runtime.");
                    entry.fit.executionSuitability = QStringLiteral("CPU path may be available; GPU offload unknown.");
                }
                if (!entry.localFile.isEmpty() && entry.sizeBytes &&
                    hardware_.facts().systemRamBytes &&
                    *entry.sizeBytes > *hardware_.facts().systemRamBytes) {
                    entry.fit.state = ModelCompatibility::ResourceConstrained;
                    entry.fit.reasons << QStringLiteral("Artifact exceeds reported system RAM; execution may require paging.");
                }
            }
        } else if (entry.format.compare(QStringLiteral("GGUF"), Qt::CaseInsensitive) == 0) {
            entry.fit.reasons << QStringLiteral("No configured llama.cpp runtime was found.");
        }
        if (entry.fit.state == ModelCompatibility::Unknown && entry.fit.reasons.isEmpty())
            entry.fit.reasons << QStringLiteral("Resource requirements or runtime support are not reported.");
        entry.actions = availableActions(entry);
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    return result;
}

void ModelLibraryService::setStorageManager(const ModelStorageManager* storage) {
    storage_ = storage;
}

void ModelLibraryService::setRuntimeStatus(const ModelRuntimeStatus& status) {
    if (!status.runtimeId.isEmpty()) runtimeStatuses_.insert(status.runtimeId, status);
}

const HardwareFacts& ModelLibraryService::hardwareFacts() const { return hardware_.facts(); }
void ModelLibraryService::refreshHardwareFacts() { hardware_.refresh(); }

QList<ModelLibraryEntry> ModelLibraryService::query(const ModelLibraryQuery& filter) const {
    QList<ModelLibraryEntry> result;
    for (const auto& entry : entries()) {
        if (!filter.text.isEmpty() && !entry.displayName.contains(filter.text, Qt::CaseInsensitive) &&
            !entry.repositoryId.contains(filter.text, Qt::CaseInsensitive)) continue;
        if (!filter.providerIds.isEmpty() && !filter.providerIds.contains(entry.provider.id)) continue;
        if (!filter.runtimeIds.isEmpty() && !filter.runtimeIds.contains(entry.runtime.id)) continue;
        if (filter.local && entry.local != *filter.local) continue;
        if (filter.installed &&
            (entry.installed == ModelLibraryInstalledState::Unknown ||
             (entry.installed == ModelLibraryInstalledState::Installed) != *filter.installed)) continue;
        if (filter.available &&
            (entry.availability == ModelLibraryAvailability::Unknown ||
             (entry.availability == ModelLibraryAvailability::Available) != *filter.available)) continue;
        if (!matchesSupport(entry.capabilities.nativeToolCalling, filter.toolCalling) ||
            !matchesSupport(entry.capabilities.structuredOutput, filter.structuredOutput) ||
            !matchesSupport(entry.capabilities.visionInput, filter.vision) ||
            !matchesSupport(entry.capabilities.audioInput, filter.audio)) continue;
        if (!filter.family.isEmpty() && entry.family.compare(filter.family, Qt::CaseInsensitive)) continue;
        if (!filter.format.isEmpty() && entry.format.compare(filter.format, Qt::CaseInsensitive)) continue;
        if (!filter.quantization.isEmpty() &&
            entry.quantization.compare(filter.quantization, Qt::CaseInsensitive)) continue;
        if (!filter.publisher.isEmpty() &&
            entry.publisher.compare(filter.publisher, Qt::CaseInsensitive)) continue;
        if (!filter.architecture.isEmpty() &&
            entry.architecture.compare(filter.architecture, Qt::CaseInsensitive)) continue;
        if (filter.gguf && (entry.format.compare(QStringLiteral("GGUF"), Qt::CaseInsensitive) == 0) != *filter.gguf)
            continue;
        if (filter.llamaCppCandidate &&
            (entry.format.compare(QStringLiteral("GGUF"), Qt::CaseInsensitive) == 0 &&
             entry.shardGroup.isEmpty()) != *filter.llamaCppCandidate) continue;
        result.append(entry);
    }
    return result;
}

} // namespace sentinel::core
