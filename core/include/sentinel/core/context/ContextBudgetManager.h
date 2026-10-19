// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/interfaces/IChatProvider.h"
#include "sentinel/core/memory/ScopedMemory.h"
#include "sentinel/core/model/ModelRouting.h"
#include <functional>

namespace sentinel::core {
enum class ContextBudgetStatus {
    Ready,
    Reduced,
    InsufficientCapacity,
    InvalidMetadata,
    InvalidContinuation,
    MultimodalUncertain
};
enum class ContextBudgetKind { Instruction, Conversation, Memory, Observation, Tool, Other };
enum class ContextOmissionReason { None, Capacity, Unauthorized, InvalidMetadata };
struct ContextBudgetItem {
    QString text;
    ContextBudgetKind kind = ContextBudgetKind::Other;
    int priority = 0; // Higher first; stable input order breaks ties.
    bool required = false;
    ContextOmissionReason exclusion = ContextOmissionReason::None;
};
struct ContextBudgetDecision {
    int index = 0;
    ContextBudgetKind kind = ContextBudgetKind::Other;
    bool included = false;
    ContextOmissionReason reason = ContextOmissionReason::None;
};
struct ContextBudgetIdentity {
    QString sessionId;
    QString workspaceId;
};
struct ContextBudgetResult {
    ContextBudgetStatus status = ContextBudgetStatus::InvalidMetadata;
    ContextBudgetIdentity identity;
    ModelBinding binding; // Snapshot only; never resolves or redirects a model.
    std::optional<int> reportedCapacity;
    ModelMetadataSource capacitySource = ModelMetadataSource::Unknown;
    int effectiveCapacity = 8192;
    int reservedOutput = 2048;
    std::optional<qint64> estimatedInput;
    std::optional<qint64> uncertaintyAllowance;
    std::optional<qint64> estimatedRequiredInput;
    std::optional<qint64> estimatedToolSchemas;
    std::optional<qint64> estimatedContinuation;
    QList<ContextBudgetDecision> decisions;
    QString prompt;
    bool accepted() const {
        return status == ContextBudgetStatus::Ready || status == ContextBudgetStatus::Reduced;
    }
    QJsonObject diagnostics() const; // Counts/status only, no IDs or private data.
};
class ContextBudgetManager final {
public:
    static constexpr int UnknownCapacity = 8192;
    static constexpr int DefaultOutputReserve = 2048;
    // Policy estimate, not tokenizer measurement: ceil(UTF-8 bytes/3), plus 25% uncertainty.
    // Request JSON escaping and a 256-token framing floor are accounted separately.
    static qint64 estimateText(const QString& text);
    using Renderer = std::function<QString(const QList<int>&)>;
    ContextBudgetResult assemble(const ModelBinding& binding, const ChatRequestOptions& options,
                                 const QList<ContextBudgetItem>& items, const Renderer& render = {},
                                 const ContextBudgetIdentity& identity = {}) const;
    static ContextBudgetItem memoryItem(const ScopedMemoryRecord& record,
                                        const MemoryAccessContext& access, int priority = 0);
};
} // namespace sentinel::core
