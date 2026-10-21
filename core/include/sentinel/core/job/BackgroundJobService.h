// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/app/StorageAdmission.h"
#include "sentinel/core/job/IJobService.h"
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QQueue>
#include <QSqlDatabase>
#include <QThreadPool>
#include <QUuid>

namespace sentinel::core {

class BackgroundJobService : public QObject, public IJobService {
    Q_OBJECT
public:
    explicit BackgroundJobService(QObject* parent = nullptr);
    ~BackgroundJobService() override;

    // IJobService interface
    QString submitJob(const QString& name, JobFunction func,
                      const QString& description = {}) override;
    bool cancelJob(const QString& jobId) override;
    bool retryJob(const QString& jobId) override;
    bool removeJob(const QString& jobId) override;

    std::optional<Job> findJob(const QString& jobId) const override;
    QList<Job> jobs() const override;
    QList<Job> activeJobs() const override;
    QList<Job> completedJobs() const override;
    QList<Job> failedJobs() const override;

    void setMaxConcurrentJobs(int maxJobs) override;
    int maxConcurrentJobs() const override;

    bool setPersistenceDatabase(QSqlDatabase database,
                                std::shared_ptr<StorageAdmission> admission = {});
    int recoverPersistedJobs();

signals:
    void jobSubmitted(const QString& jobId, const QString& name);
    void jobStarted(const QString& jobId);
    void jobCompleted(const QString& jobId);
    void jobFailed(const QString& jobId, const QString& error);
    void jobCancelled(const QString& jobId);
    void jobProgressUpdated(const QString& jobId, const JobProgress& progress);

private slots:
    void processQueue();

private:
    void executeJob(const QString& jobId);
    QString generateJobId() const;

    int m_maxConcurrentJobs{4};
    QThreadPool m_threadPool;
    QMap<QString, Job> m_jobs;
    QMap<QString, JobFunction> m_jobFunctions;
    QQueue<QString> m_jobQueue;
    QMutex m_mutex;
    int m_runningJobs{0};
    QSqlDatabase m_database;

    bool initializePersistence();
    bool persistJob(const Job& job);
    std::shared_ptr<StorageAdmission> admission_;
    std::uint64_t generation_ = 0;
    QMap<QString, std::shared_ptr<StorageAdmission::Writer>> operationLeases_;
};

} // namespace sentinel::core
