// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/agent/ObservationEvidence.h"
#include <optional>

namespace sentinel::core {

enum class ClaimVerdict { Supported, Contradicted, Unknown };
struct ClaimResolution {
    ClaimVerdict verdict = ClaimVerdict::Unknown;
    StructuredFact fact;
};

struct EvidenceExplanation {
    QString canonical;
    QString rendered;
};

class ClaimGroundingResolver final {
public:
    static std::optional<EvidenceExplanation>
    filesystemExplanation(const ObservationIntent& intent, const QList<EvidenceRecord>& evidence,
                          const QString& proposed);
    static std::optional<QString> filesystemFinalAnswer(const ObservationIntent& intent,
                                                        const QList<EvidenceRecord>& evidence,
                                                        const QString& proposed);
    // True when the current observations cannot safely support a natural-language
    // absence or completeness conclusion. Callers must keep the scoped,
    // evidence-derived representation in that case.
    static bool canonicalObservationRequired(const QList<EvidenceRecord>& evidence);
    static ClaimResolution resolve(const ObservationRequirement& claim,
                                   const QList<EvidenceRecord>& evidence,
                                   const ClaimAssertion& assertion);
    static QList<StructuredFact> facts(const ObservationIntent& intent,
                                       const QList<EvidenceRecord>& evidence);
};
} // namespace sentinel::core
