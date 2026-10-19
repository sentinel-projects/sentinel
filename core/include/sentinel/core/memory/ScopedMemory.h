// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <cstdint>
#include <optional>
namespace sentinel::core {
enum class MemoryScope : std::uint8_t { Legacy = 0, Session = 1, Project = 2, Personal = 3 };
enum class MemoryProvenance : std::uint8_t {
    Unknown = 0,
    UserProvided = 1,
    ConversationContent = 2,
    ProjectDerived = 3,
    RetrievalResult = 4,
    GeneratedSummary = 5,
    Inferred = 6
};
enum class ScopedMemoryStatus : std::uint8_t { Ready, Unavailable, Unauthorized, InvalidInput };
// Internal, trusted caller context. Never deserialize these fields from model/tool arguments.
// Workspace authority is resolved by the application/session owner, not relevance scoring.
struct MemoryAccessContext {
    QString sessionId;
    QString workspaceId;
    QSet<QString> authorizedWorkspaceIds;
    bool includePersonal = false;
    std::optional<MemoryScope> retrievalScope; // Restriction only; never grants authority.
};
struct ScopedMemoryRecord {
    QString id;
    MemoryScope scope = MemoryScope::Legacy;
    QString ownerId;
    QString workspaceId;
    QString key;
    QString value;
    MemoryProvenance provenance = MemoryProvenance::Unknown;
    QString sourceReference;
    QDateTime createdAtUtc;
    QDateTime updatedAtUtc;
    QString revision;
    QJsonArray corrections;
};
struct ScopedMemoryWrite {
    MemoryScope scope = MemoryScope::Session;
    QString key;
    QString value;
    MemoryProvenance provenance = MemoryProvenance::Unknown;
    QString sourceReference;
    bool overwriteExisting = true;
};
enum class MemoryIndexState : std::uint8_t { Unavailable, Ready, Stale, Direct };
struct MemoryIndexStatus {
    MemoryIndexState state = MemoryIndexState::Unavailable;
    int refreshed = 0;
};
struct MemoryRelevance {
    QString id;
    int lexicalScore = 0;
    bool exactKey = false;
    bool phrase = false;
    int matchedTerms = 0;
    int totalTerms = 0;
};
struct ScopedMemoryResult {
    ScopedMemoryStatus status = ScopedMemoryStatus::Unavailable;
    QList<ScopedMemoryRecord> records;
    QList<MemoryRelevance> relevance;
    MemoryIndexStatus index;
    int candidatesExamined = 0;
    bool candidateLimitReached = false;
};
inline bool authorizedMemoryContext(const MemoryAccessContext& context) {
    return !context.sessionId.trimmed().isEmpty() && context.sessionId.size() <= 256 &&
           context.workspaceId.size() <= 256 &&
           (context.workspaceId.isEmpty() ||
            context.authorizedWorkspaceIds.contains(context.workspaceId));
}
inline bool permittedMemoryWrite(const ScopedMemoryWrite& write,
                                 const MemoryAccessContext& context) {
    if (!authorizedMemoryContext(context)) {
        return false;
    }
    switch (write.scope) {
    case MemoryScope::Session:
        return true;
    case MemoryScope::Project:
        return !context.workspaceId.isEmpty();
    case MemoryScope::Personal:
        return context.includePersonal;
    case MemoryScope::Legacy:
        return false;
    }
    return false;
}
inline bool validMemoryWrite(const ScopedMemoryWrite& write) {
    const auto origin = static_cast<int>(write.provenance);
    return !write.key.trimmed().isEmpty() && write.key.size() <= 256 &&
           !write.value.trimmed().isEmpty() && write.value.size() <= 65536 &&
           write.sourceReference.size() <= 512 && origin >= 0 &&
           origin <= static_cast<int>(MemoryProvenance::Inferred);
}
inline QString memoryOwner(MemoryScope scope, const MemoryAccessContext& context) {
    return scope == MemoryScope::Session   ? context.sessionId
           : scope == MemoryScope::Project ? context.workspaceId
                                           : QString{};
}
inline QString memoryWorkspace(MemoryScope scope, const MemoryAccessContext& context) {
    return scope == MemoryScope::Personal ? QString{} : context.workspaceId;
}
inline bool visibleMemory(const ScopedMemoryRecord& record, const MemoryAccessContext& context) {
    if (!authorizedMemoryContext(context)) {
        return false;
    }
    if (context.retrievalScope && record.scope != *context.retrievalScope) {
        return false;
    }
    switch (record.scope) {
    case MemoryScope::Session:
        return record.ownerId == context.sessionId && record.workspaceId == context.workspaceId;
    case MemoryScope::Project:
        return !context.workspaceId.isEmpty() && record.ownerId == context.workspaceId &&
               record.workspaceId == context.workspaceId;
    case MemoryScope::Personal:
        return context.includePersonal && record.ownerId.isEmpty() && record.workspaceId.isEmpty();
    case MemoryScope::Legacy:
        return false;
    }
    return false;
}
QString memoryIndexStateName(MemoryIndexState);
MemoryRelevance memoryRelevance(const ScopedMemoryRecord&, const QString&);
ScopedMemoryResult rankScopedMemory(QList<ScopedMemoryRecord>, const QString&, int,
                                    MemoryIndexStatus = {});
inline constexpr int memoryCandidateLimit = 200;
bool isImportedMemory(const ScopedMemoryRecord&);
QStringList scopedMemoryQueryTerms(const QString& text);
QString memoryScopeName(MemoryScope scope);
QString memoryProvenanceName(MemoryProvenance provenance);
QString scopedMemoryContextText(const ScopedMemoryResult& result);
} // namespace sentinel::core
