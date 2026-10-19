// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/agent/ObservationEvidence.h"
#include "sentinel/core/model/ModelRouting.h"
#include "sentinel/core/runtime/ToolDescriptor.h"
#include "sentinel/core/runtime/ToolExecution.h"
#include <atomic>
#include <memory>
#include <utility>

namespace sentinel::core {
class IChatProvider;
QString normalizedObservationResource(const QString& resource, ObservationDomain domain);

class IObservationIntentPolicy {
public:
    virtual ~IObservationIntentPolicy() = default;
    virtual ObservationIntent classify(const QString& goal, const QString& conversationContext,
                                       const QList<ToolDescriptor>& tools) const = 0;
};

// Separate from the action planner. Its compact semantic classification is
// checked against descriptor metadata and never itself counts as evidence.
class ObservationIntentPolicy final : public IObservationIntentPolicy {
public:
    explicit ObservationIntentPolicy(IChatProvider* provider,
                                     std::shared_ptr<std::atomic_bool> cancellationToken = {},
                                     ModelBinding binding = {})
        : binding_(std::move(binding)), provider_(provider),
          cancellationToken_(std::move(cancellationToken)) {}
    ObservationIntent classify(const QString& goal, const QString& conversationContext,
                               const QList<ToolDescriptor>& tools) const override;

private:
    ModelBinding binding_;
    IChatProvider* provider_ = nullptr;
    std::shared_ptr<std::atomic_bool> cancellationToken_;
};

struct EvidenceGateResult {
    bool accepted = false;
    QString repair;
    QString answerOverride;
    FinalAnswerGrounding grounding;
};

class EvidencePolicy final {
public:
    static QList<EvidenceRecord> record(const ToolDescriptor& descriptor,
                                        const PlannedToolInvocation& invocation,
                                        ToolExecutionStatus status, const QString& summary,
                                        int stepIndex,
                                        const QString& toolCallId, StructuredObservationPtr structuredObservation = {});
    static EvidenceGateResult evaluate(const ObservationIntent& intent,
                                       const QList<EvidenceRecord>& evidence,
                                       GroundingMode mode, bool groundingDeclared);
};
} // namespace sentinel::core
