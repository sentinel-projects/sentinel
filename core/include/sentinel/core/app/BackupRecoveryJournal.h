// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
class QIODevice;
namespace sentinel::core {

// Internal persistence primitive. It does not admit, apply or replay a restore.
// Callers must already own the profile identity and quiescent domain readers.
enum class BackupRecoveryState : std::uint8_t {
    Preflight,
    Validated,
    Prepared,
    Applying,
    Committed,
    RolledBack,
    RecoveryRequired
};
enum class BackupRecoveryDomain : std::uint8_t { Memory = 1, Conversations = 2 };
struct BackupRecoveryPreimage {
    BackupRecoveryDomain domain;
    int schemaVersion;
    QIODevice* source;
    QByteArray expectedRestoredSha256;
};
struct BackupRecoveryVerification {
    BackupRecoveryDomain domain;
    QIODevice* source;
};
class BackupRecoveryJournal final {
public:
    // The profile UUID is trusted composition input, never taken from an archive/journal.
    BackupRecoveryJournal(QString profileDirectory, QString profileId);
    ~BackupRecoveryJournal();
    BackupRecoveryJournal(const BackupRecoveryJournal&) = delete;
    BackupRecoveryJournal& operator=(const BackupRecoveryJournal&) = delete;
    QString open();
    QString begin(const QString& operationId, const QByteArray& archiveSha256);
    QString validated();
    QString prepare(const QList<BackupRecoveryPreimage>&,
                    const std::function<bool()>& cancelled = {});
    QString applying();
    QString recoveryRequired();
    // Both terminal decisions independently read all provided authoritative domains.
    QString finish(bool rollback, const QList<BackupRecoveryVerification>&);
    QJsonObject inspect();
    // Only copies a verified preimage into caller-owned private staging; never applies it.
    QString readPreimage(BackupRecoveryDomain, QIODevice& destination);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
QString backupRecoveryStateName(BackupRecoveryState);
} // namespace sentinel::core
