// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/memory/MemoryMetadata.h"

#include <QDateTime>
#include <QList>
#include <QString>
#include <QStringList>

#include <QtGlobal>
#include <cstdint>

namespace sentinel::core {

using MemoryCandidateId = QString;

enum class MemoryCandidateSource : std::uint8_t {
    ConversationText,
    ManualMetadata,
};

enum class MemoryCandidateCategory : std::uint8_t {
    Semantic,
    Preference,
    ProjectContext,
    SafetyNote,
};

enum class MemoryCandidateConfidence : std::uint8_t {
    Low,
    Medium,
    High,
};

enum class MemoryReviewState : std::uint8_t {
    PendingReview,
    Approved,
    Rejected,
    Archived,
};

enum class MemoryCandidateReviewAction : std::uint8_t {
    Approve,
    Reject,
    ResetToPending,
    Archive,
};

enum class MemoryCapturePolicy : std::uint8_t {
    MetadataOnly,
    ManualReviewRequired,
    Disabled,
};

enum class MemoryCommitTarget : std::uint8_t {
    KeyValueMemory,
};

enum class MemoryCommitReadinessStatus : std::uint8_t {
    Disabled,
    Ready,
    PendingReview,
    Rejected,
    Archived,
    MissingCandidate,
    StoreUnavailable,
    AlreadyCommitted,
};

enum class MemoryCommitStatus : std::uint8_t {
    NotCommitted,
    Committed,
    Refused,
};

enum class MemoryCommitConflictPolicy : std::uint8_t {
    Refuse,
    Overwrite,
};

QString memoryCandidateSourceName(MemoryCandidateSource source);
QString memoryCandidateCategoryName(MemoryCandidateCategory category);
QString memoryCandidateConfidenceName(MemoryCandidateConfidence confidence);
QString memoryReviewStateName(MemoryReviewState state);
QString memoryCandidateReviewActionName(MemoryCandidateReviewAction action);
QString memoryCapturePolicyName(MemoryCapturePolicy policy);
QString memoryCommitTargetName(MemoryCommitTarget target);
QString memoryCommitReadinessStatusName(MemoryCommitReadinessStatus status);
QString memoryCommitStatusName(MemoryCommitStatus status);
QString memoryCommitConflictPolicyName(MemoryCommitConflictPolicy policy);

struct MemoryCandidate {
    MemoryCandidateId id;
    MemoryCandidateSource source = MemoryCandidateSource::ConversationText;
    MemoryCandidateCategory category = MemoryCandidateCategory::Semantic;
    MemoryCandidateConfidence confidence = MemoryCandidateConfidence::Medium;
    MemoryReviewState reviewState = MemoryReviewState::PendingReview;
    MemoryRetentionPolicy retentionPolicy = MemoryRetentionPolicy::UserControlled;
    MemoryCapturePolicy capturePolicy = MemoryCapturePolicy::ManualReviewRequired;
    QString title;
    QString excerpt;
    QString sourceSummary = QStringLiteral("Conversation text metadata only.");
    QString reviewSummary = QStringLiteral("Pending user review.");
    QString reviewerSummary;
    QString decisionReason;
    MemoryCommitStatus commitStatus = MemoryCommitStatus::NotCommitted;
    QString committedKey;
    QString commitSummary = QStringLiteral("Not committed.");
    QDateTime createdAtUtc;
    QDateTime reviewedAtUtc;
    QDateTime committedAtUtc;
    QString sourceCaptureId;
    QString sourceSessionId;
    QString sourceWorkspaceId;
};

struct MemoryCandidateSummary {
    MemoryCandidateId id;
    QString title;
    QString state;
    QString category;
    QString confidence;
    QString retention;
    QString source;
    QString reviewedAt;
    QString reviewer;
    QString commitStatus;
    QString committedAt;
    QString committedKey;
    QString summary;
};

MemoryCandidateSummary memoryCandidateSummary(const MemoryCandidate& candidate);

struct MemoryCandidateReviewResult {
    bool accepted = false;
    MemoryCandidateId candidateId;
    MemoryCandidateReviewAction action = MemoryCandidateReviewAction::Approve;
    MemoryReviewState previousState = MemoryReviewState::PendingReview;
    MemoryReviewState newState = MemoryReviewState::PendingReview;
    QDateTime reviewedAtUtc;
    QString reviewerSummary;
    QString sourceSummary;
    QString decisionReason;
    QString status;
    QString summary;
};

struct MemoryCommitPolicy {
    bool explicitUserCommitAllowed = true;
    MemoryCommitTarget target = MemoryCommitTarget::KeyValueMemory;
    MemoryCommitConflictPolicy conflictPolicy = MemoryCommitConflictPolicy::Refuse;
    QString status = QStringLiteral("Explicit User Action");
    QString summary = QStringLiteral(
        "Memory commit requires an approved candidate and an explicit user Commit action.");
};

struct MemoryCommitPlan {
    MemoryCandidateId candidateId;
    MemoryCommitTarget target = MemoryCommitTarget::KeyValueMemory;
    QString key;
    QString valuePreview;
    QString targetSummary;
    QString summary;
};

struct MemoryCommitReadiness {
    bool ready = false;
    MemoryCandidateId candidateId;
    MemoryCommitReadinessStatus status = MemoryCommitReadinessStatus::Disabled;
    MemoryCommitPlan plan;
    QString summary;
    QStringList checks;
};

struct MemoryCommitResult {
    bool accepted = false;
    MemoryCandidateId candidateId;
    MemoryCommitTarget target = MemoryCommitTarget::KeyValueMemory;
    MemoryCommitStatus commitStatus = MemoryCommitStatus::NotCommitted;
    QDateTime requestedAtUtc;
    QDateTime committedAtUtc;
    QString committedKey;
    QString status;
    QString summary;
};

MemoryCommitPlan memoryCommitPlanForCandidate(const MemoryCandidate& candidate,
                                              MemoryCommitTarget target);
MemoryCommitReadiness memoryCommitReadinessForCandidate(const MemoryCandidate* candidate,
                                                        bool memoryStoreAvailable,
                                                        const MemoryCommitPolicy& policy);

class IMemoryCandidateStore {
public:
    Q_DISABLE_COPY(IMemoryCandidateStore)
    IMemoryCandidateStore() = default;
    virtual ~IMemoryCandidateStore() = default;

    virtual MemoryCandidate createCandidate(MemoryCandidate candidate) = 0;
    virtual QList<MemoryCandidate> candidates() const = 0;
    virtual MemoryCandidateReviewResult reviewCandidate(const MemoryCandidateId& id,
                                                        MemoryCandidateReviewAction action,
                                                        const QString& reviewerSummary,
                                                        const QString& decisionReason) = 0;
    virtual bool recordCommitResult(const MemoryCommitResult& result) = 0;
};

} // namespace sentinel::core
