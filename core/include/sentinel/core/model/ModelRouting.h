// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace sentinel::core {

enum class ProviderKind {
    Local,
    Cloud,
};

enum class RoutingMode {
    Auto,
    Fast,
    Balanced,
    Quality,
    LocalOnly,
    CloudAllowed,
    BatterySaver,
};

enum class TaskType {
    Unknown,
    Chat,
    Summarization,
    Coding,
    Planning,
    LongContext,
    ToolPlanning,
    SensitiveData,
};

enum class ModelRoutingStatus {
    Unavailable,
    Routed,
    NoAvailableModel,
};

enum class CapabilitySupport { Unknown, Unsupported, Supported };

enum class ModelMetadataSource {
    Unknown,
    ProviderDefault,
    ProviderCatalog,
    ProviderOwnedMetadata,
    RuntimeReported,
    ProgrammaticMetadata,
    UserOverride,
};

// What a reported context window actually describes. A model's published
// maximum is a property of the weights; the capacity a server loaded the model
// with is a property of the running runtime. Token admission must never treat
// the first as the second: llama.cpp can be launched with -c 8192 for a model
// whose trained context is 131072, and only the server knows which one is in
// effect for the current request.
enum class ContextCapacityKind {
    Unknown,
    // Published/trained maximum context for the model. An upper bound only.
    ModelMaximum,
    // The context the server actually configured for the loaded model
    // (llama.cpp /props n_ctx, LM Studio loaded_instances context_length,
    // Ollama /api/ps context). Authoritative for admission.
    ServerConfigured,
};

inline QString contextCapacityKindName(ContextCapacityKind kind) {
    switch (kind) {
    case ContextCapacityKind::ServerConfigured:
        return QStringLiteral("server_configured");
    case ContextCapacityKind::ModelMaximum:
        return QStringLiteral("model_maximum");
    case ContextCapacityKind::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

// A more specific observation supersedes a vaguer one when capability snapshots
// are merged.
inline ContextCapacityKind tighterContextCapacityKind(ContextCapacityKind base,
                                                      ContextCapacityKind candidate) {
    if (candidate == ContextCapacityKind::ServerConfigured)
        return ContextCapacityKind::ServerConfigured;
    if (candidate == ContextCapacityKind::ModelMaximum &&
        base != ContextCapacityKind::ServerConfigured)
        return ContextCapacityKind::ModelMaximum;
    return base;
}

struct ModelCapabilityProvenance {
    ModelMetadataSource streaming = ModelMetadataSource::Unknown;
    ModelMetadataSource structuredOutput = ModelMetadataSource::Unknown;
    ModelMetadataSource nativeToolCalling = ModelMetadataSource::Unknown;
    ModelMetadataSource combinedToolsAndStructuredOutput = ModelMetadataSource::Unknown;
    ModelMetadataSource visionInput = ModelMetadataSource::Unknown;
    ModelMetadataSource audioInput = ModelMetadataSource::Unknown;
    ModelMetadataSource audioOutput = ModelMetadataSource::Unknown;
    ModelMetadataSource contextWindow = ModelMetadataSource::Unknown;
    ModelMetadataSource maxOutputTokens = ModelMetadataSource::Unknown;
    // Provenance for `contextCapacity`, kept separate so a catalog maximum and a
    // runtime-reported loaded capacity can be told apart even when both describe
    // `contextWindow`.
    ModelMetadataSource contextCapacity = ModelMetadataSource::Unknown;
};

struct ModelCapabilities {
    CapabilitySupport streaming = CapabilitySupport::Unknown;
    CapabilitySupport structuredOutput = CapabilitySupport::Unknown;
    CapabilitySupport nativeToolCalling = CapabilitySupport::Unknown;
    CapabilitySupport combinedToolsAndStructuredOutput = CapabilitySupport::Unknown;
    CapabilitySupport visionInput = CapabilitySupport::Unknown;
    CapabilitySupport audioInput = CapabilitySupport::Unknown;
    CapabilitySupport audioOutput = CapabilitySupport::Unknown;
    std::optional<int> contextWindow;
    std::optional<int> maxOutputTokens;
    // What `contextWindow` describes. Admission uses a ServerConfigured value;
    // a ModelMaximum value is an upper bound and is reported as such.
    ContextCapacityKind contextCapacity = ContextCapacityKind::Unknown;
    ModelCapabilityProvenance provenance;
};

inline QString capabilitySnapshotSummary(const ModelCapabilities& capabilities) {
    auto state = [](CapabilitySupport value) {
        switch (value) {
        case CapabilitySupport::Supported: return QStringLiteral("yes");
        case CapabilitySupport::Unsupported: return QStringLiteral("no");
        case CapabilitySupport::Unknown: return QStringLiteral("unknown");
        }
        return QStringLiteral("unknown");
    };
    return QStringLiteral("Context %1, streaming %2, structured %3, native tools %4")
        .arg(capabilities.contextWindow ? QString::number(*capabilities.contextWindow)
                                        : QStringLiteral("unknown"),
             state(capabilities.streaming), state(capabilities.structuredOutput),
             state(capabilities.nativeToolCalling));
}

inline QString providerKindName(ProviderKind kind) {
    switch (kind) {
    case ProviderKind::Local:
        return QStringLiteral("Local");
    case ProviderKind::Cloud:
        return QStringLiteral("Cloud");
    }

    return QStringLiteral("Local");
}

inline QString routingModeName(RoutingMode mode) {
    switch (mode) {
    case RoutingMode::Auto:
        return QStringLiteral("Auto");
    case RoutingMode::Fast:
        return QStringLiteral("Fast");
    case RoutingMode::Balanced:
        return QStringLiteral("Balanced");
    case RoutingMode::Quality:
        return QStringLiteral("Quality");
    case RoutingMode::LocalOnly:
        return QStringLiteral("Local Only");
    case RoutingMode::CloudAllowed:
        return QStringLiteral("Cloud Allowed");
    case RoutingMode::BatterySaver:
        return QStringLiteral("Battery Saver");
    }

    return QStringLiteral("Auto");
}

inline QStringList routingModeNames() {
    return {
        routingModeName(RoutingMode::Auto),         routingModeName(RoutingMode::Fast),
        routingModeName(RoutingMode::Balanced),     routingModeName(RoutingMode::Quality),
        routingModeName(RoutingMode::LocalOnly),    routingModeName(RoutingMode::CloudAllowed),
        routingModeName(RoutingMode::BatterySaver),
    };
}

inline RoutingMode routingModeFromName(const QString& name) {
    const auto normalized = name.trimmed();
    if (normalized == routingModeName(RoutingMode::Auto)) {
        return RoutingMode::Auto;
    }
    if (normalized == routingModeName(RoutingMode::Fast)) {
        return RoutingMode::Fast;
    }
    if (normalized == routingModeName(RoutingMode::Balanced)) {
        return RoutingMode::Balanced;
    }
    if (normalized == routingModeName(RoutingMode::Quality)) {
        return RoutingMode::Quality;
    }
    if (normalized == routingModeName(RoutingMode::CloudAllowed)) {
        return RoutingMode::CloudAllowed;
    }
    if (normalized == routingModeName(RoutingMode::BatterySaver)) {
        return RoutingMode::BatterySaver;
    }
    return RoutingMode::LocalOnly;
}

inline QString normalizedRoutingModeName(const QString& name) {
    return routingModeName(routingModeFromName(name));
}

inline QString taskTypeName(TaskType type) {
    switch (type) {
    case TaskType::Unknown:
        return QStringLiteral("Unknown");
    case TaskType::Chat:
        return QStringLiteral("Chat");
    case TaskType::Summarization:
        return QStringLiteral("Summarization");
    case TaskType::Coding:
        return QStringLiteral("Coding");
    case TaskType::Planning:
        return QStringLiteral("Planning");
    case TaskType::LongContext:
        return QStringLiteral("Long Context");
    case TaskType::ToolPlanning:
        return QStringLiteral("Tool Planning");
    case TaskType::SensitiveData:
        return QStringLiteral("Sensitive Data");
    }

    return QStringLiteral("Unknown");
}

inline QString modelRoutingStatusName(ModelRoutingStatus status) {
    switch (status) {
    case ModelRoutingStatus::Unavailable:
        return QStringLiteral("Unavailable");
    case ModelRoutingStatus::Routed:
        return QStringLiteral("Routed");
    case ModelRoutingStatus::NoAvailableModel:
        return QStringLiteral("No Available Model");
    }

    return QStringLiteral("Unavailable");
}

struct ProviderCapabilityProfile {
    bool local = true;
    bool cloud = false;
    bool supportsChat = true;
    bool supportsTools = false;
    bool sensitiveDataAllowed = true;
    int contextWindowTokens = 0;
    QString latencyClass;
    QString costClass;
    QString privacyPosture;
    QStringList supportedTaskTypes;
};

struct ProviderDescriptor {
    QString id;
    QString name;
    ProviderKind kind = ProviderKind::Local;
    ProviderCapabilityProfile capabilityProfile;
    ModelCapabilities modelCapabilities;
};

struct ModelDescriptor {
    QString id;
    QString name;
    QString providerId;
    bool localOnly = true;
    bool installed = true;
    int contextWindowTokens = 0;
    QString qualityClass;
    QString latencyClass;
    QStringList recommendedTaskTypes;
    ModelCapabilities capabilities;
};

struct TaskClassification {
    TaskType type = TaskType::Unknown;
    bool sensitive = false;
    QString summary;
};

struct ModelRoute {
    ModelRoutingStatus status = ModelRoutingStatus::Unavailable;
    RoutingMode routingMode = RoutingMode::LocalOnly;
    TaskClassification task;
    ProviderDescriptor provider;
    ModelDescriptor model;
    QString summary;
    bool networkRequired = false;
    bool modelExecutionAllowed = false;
};

struct ModelBinding {
    QString providerId;
    QString modelId;
    int contextWindowTokens = 0;
    ModelCapabilities capabilities;

    bool isConfigured() const { return !providerId.isEmpty() && !modelId.isEmpty(); }
};

inline QString safeModelRouteSummary(const ModelRoute& route) {
    if (route.status != ModelRoutingStatus::Routed) {
        return route.summary.isEmpty() ? modelRoutingStatusName(route.status) : route.summary;
    }

    if (!route.summary.isEmpty()) {
        return route.summary;
    }

    return QStringLiteral("%1 -> %2 / %3")
        .arg(routingModeName(route.routingMode), route.provider.name, route.model.name);
}

} // namespace sentinel::core
