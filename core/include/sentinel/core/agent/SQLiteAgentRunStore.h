// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sentinel/core/app/StorageAdmission.h"

#include "sentinel/core/agent/IAgentRunStore.h"

#include <mutex>

namespace sentinel::core {

class SQLiteAgentRunStore final : public IAgentRunStore {
public:
    explicit SQLiteAgentRunStore(QString databasePath,
                                 std::shared_ptr<StorageAdmission> admission = {},
                                 std::uint64_t generation = 0);
    bool clearHistory() override;
    int pruneCompletedBefore(const QDateTime& cutoffUtc, int limit = 100) override;
    bool record(const AgentEvent& event) override;
    QList<StoredAgentRun> recentRuns(int limit) const override;
    QList<StoredAgentRun> runsBefore(const QDateTime& startedAt,
                                     const QString& runId, int limit) const override;
    StoredAgentRun runById(const QString& runId) const override;
    QList<StoredAgentRun> childRuns(const QString& parentRunId, int limit) const override;
    QList<StoredAgentStep> stepsForRun(const QString& runId, int limit) const override;
    QList<StoredAgentToolCall> toolCallsForRun(const QString& runId, int limit) const override;
    QList<StoredAgentEvidence> evidenceForRun(const QString& runId, int limit) const override;
    QList<StoredAgentClaim> claimsForRun(const QString& runId, int limit) const override;
    QList<StoredAgentAuthorization> authorizationsForRun(const QString& runId,
                                                         int limit) const override;
    bool coordinatedBy(const std::shared_ptr<StorageAdmission>& authority) const override {
        return authority && admission_ == authority;
    }
    QString lastError() const override;

private:
    bool initialize();
    StorageAdmission::Writer admitWrite();
    std::shared_ptr<StorageAdmission> admission_;
    const std::uint64_t generation_;
    QString databasePath_;
    mutable std::mutex mutex_;
    mutable QString lastError_;
    bool ready_ = false;
};

} // namespace sentinel::core
