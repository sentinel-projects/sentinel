// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "sentinel/core/app/StorageAdmission.h"
#include "sentinel/core/memory/MemoryArchive.h"
#include <QHash>
#include <QJsonObject>
#include <memory>
class QLockFile;
class QObject;
namespace sentinel::core {
class MemoryArchiveJobs final {
public:
    explicit MemoryArchiveJobs(QString databasePath, QObject* owner = nullptr,
                               std::function<void()> restored = {},
                               std::shared_ptr<StorageAdmission> admission = {},
                               std::uint64_t generation = 0);
    ~MemoryArchiveJobs();
    QJsonObject handle(const QJsonObject&, MemoryScope, const MemoryManagementContext&);

private:
    struct Job;
    QJsonObject snapshot(const std::shared_ptr<Job>&) const;
    void run(const std::shared_ptr<Job>&, const QString&, bool = false);
    std::shared_ptr<StorageAdmission> admission_;
    std::uint64_t generation_ = 0;
    QString databasePath_, directory_;
    std::unique_ptr<QLockFile> lock_;
    bool available_ = false;
    QObject* owner_ = nullptr;
    std::function<void()> restored_;
    QHash<QString, std::shared_ptr<Job>> jobs_;
};
} // namespace sentinel::core
