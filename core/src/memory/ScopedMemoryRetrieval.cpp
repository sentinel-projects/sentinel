// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/ScopedMemory.h"
#include <algorithm>
namespace sentinel::core {
QString memoryIndexStateName(MemoryIndexState state) {
    switch (state) {
    case MemoryIndexState::Ready:
        return "Ready";
    case MemoryIndexState::Stale:
        return "Stale";
    case MemoryIndexState::Direct:
        return "Direct";
    case MemoryIndexState::Unavailable:
        return "Unavailable";
    }
    return "Unavailable";
}
MemoryRelevance memoryRelevance(const ScopedMemoryRecord& record, const QString& query) {
    const auto phrase = query.simplified().toCaseFolded();
    const auto key = record.key.simplified().toCaseFolded();
    const auto value = record.value.simplified().toCaseFolded();
    const auto terms = scopedMemoryQueryTerms(query.toCaseFolded());
    MemoryRelevance rank;
    rank.id = record.id;
    rank.totalTerms = static_cast<int>(terms.size());
    if (phrase.isEmpty()) {
        return rank;
    }
    rank.exactKey = key == phrase;
    const bool keyPhrase = key.contains(phrase), valuePhrase = value.contains(phrase);
    rank.phrase = keyPhrase || valuePhrase;
    rank.lexicalScore =
        (rank.exactKey ? 10000 : 0) + (keyPhrase ? 1200 : 0) + (valuePhrase ? 600 : 0);
    for (const auto& term : terms) {
        const bool inKey = key.contains(term), inValue = value.contains(term);
        if (inKey || inValue) {
            ++rank.matchedTerms;
        }
        rank.lexicalScore += (inKey ? 80 : 0) + (inValue ? 20 : 0);
    }
    if (!terms.isEmpty() && rank.matchedTerms == rank.totalTerms) {
        rank.lexicalScore += 200;
    }
    return rank;
}
ScopedMemoryResult rankScopedMemory(QList<ScopedMemoryRecord> records, const QString& query,
                                    int limit, MemoryIndexStatus index) {
    // Candidates have already passed the authoritative store's scope predicates.
    struct Ranked {
        ScopedMemoryRecord record;
        MemoryRelevance rank;
    };
    QList<Ranked> ranked;
    for (auto& record : records) {
        auto relevance = memoryRelevance(record, query);
        if (query.trimmed().isEmpty() || relevance.lexicalScore > 0) {
            ranked.append({std::move(record), relevance});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.rank.lexicalScore != b.rank.lexicalScore) {
            return a.rank.lexicalScore > b.rank.lexicalScore;
        }
        if (a.record.scope != b.record.scope) {
            return a.record.scope < b.record.scope;
        }
        if (a.record.updatedAtUtc != b.record.updatedAtUtc) {
            return a.record.updatedAtUtc > b.record.updatedAtUtc;
        }
        return a.record.id < b.record.id;
    });
    ScopedMemoryResult result{ScopedMemoryStatus::Ready, {}};
    result.index = index;
    result.candidatesExamined = static_cast<int>(records.size());
    result.candidateLimitReached = records.size() >= memoryCandidateLimit;
    for (const auto& item : ranked) {
        if (result.records.size() >= qBound(1, limit, 50)) {
            break;
        }
        result.records.append(item.record);
        result.relevance.append(item.rank);
    }
    return result;
}
} // namespace sentinel::core
