// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/job/BackgroundJobService.h"
#include <QDebug>
#include <QPointer>
#include <QSqlQuery>
#include <QThreadPool>

namespace sentinel::core {

BackgroundJobService::BackgroundJobService(QObject* parent) : QObject(parent) {
    m_threadPool.setMaxThreadCount(m_maxConcurrentJobs);
}

BackgroundJobService::~BackgroundJobService() {
    // Cancel all running jobs
    for (auto& job : m_jobs) {
        if (job.state == JobState::Running) {
            job.state = JobState::Cancelled;
        }
    }
    m_threadPool.waitForDone();
}

QString BackgroundJobService::submitJob(const QString& name, JobFunction func,
                                        const QString& description) {
    Job job;
    {
        QMutexLocker locker(&m_mutex);
        job.id = generateJobId();
        job.name = name;
        job.description = description;
        job.state = JobState::Queued;
        job.createdAt = QDateTime::currentDateTime();
        job.cancellationToken = std::make_shared<std::atomic_bool>(false);
        auto lifetime = std::make_shared<StorageAdmission::Writer>(
            admission_ ? admission_->admit(generation_) : StorageAdmission::Writer{});
        if (admission_ && !*lifetime)
            return {};
        if (!persistJob(job))
            return {};
        operationLeases_[job.id] = std::move(lifetime);
        m_jobs[job.id] = job;
        m_jobFunctions[job.id] = func;
        m_jobQueue.enqueue(job.id);
        emit jobSubmitted(job.id, name);
    }
    processQueue();
    return job.id;
}

bool BackgroundJobService::cancelJob(const QString& jobId) {
    QMutexLocker locker(&m_mutex);

    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end()) {
        return false;
    }

    if (it->state == JobState::Running) {
        if (it->cancellationToken) {
            it->cancellationToken->store(true);
        }
        it->state = JobState::Cancelled;
        const bool persisted = persistJob(*it);
        emit jobCancelled(jobId);
        return persisted;
    }

    if (it->state == JobState::Queued) {
        it->state = JobState::Cancelled;
        const bool persisted = persistJob(*it);
        operationLeases_.remove(jobId);
        // Remove from queue
        for (int i = 0; i < m_jobQueue.size(); ++i) {
            if (m_jobQueue[i] == jobId) {
                m_jobQueue.removeAt(i);
                break;
            }
        }
        emit jobCancelled(jobId);
        return persisted;
    }

    return false;
}

bool BackgroundJobService::retryJob(const QString& jobId) {
    {
        QMutexLocker locker(&m_mutex);
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end() || it->state != JobState::Failed ||
            it->retryCount >= it->maxRetries) {
            return false;
        }
        const Job before = *it; // NOLINT(performance-unnecessary-copy-initialization): rollback
                                // snapshot must not alias the mutable job.
        it->retryCount++;
        it->state = JobState::Queued;
        it->cancellationToken = std::make_shared<std::atomic_bool>(false);
        it->progress = JobProgress();
        it->errorString.clear();
        auto lifetime = std::make_shared<StorageAdmission::Writer>(
            admission_ ? admission_->admit(generation_) : StorageAdmission::Writer{});
        if ((admission_ && !*lifetime) || !persistJob(*it)) {
            *it = before;
            return false;
        }
        operationLeases_[jobId] = std::move(lifetime);
        m_jobQueue.enqueue(jobId);
    }
    processQueue();
    return true;
}

bool BackgroundJobService::removeJob(const QString& jobId) {
    QMutexLocker locker(&m_mutex);

    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end()) {
        return false;
    }

    if (it->state == JobState::Running) {
        return false; // Can't remove running job
    }

    operationLeases_.remove(jobId);
    m_jobs.erase(it);
    m_jobFunctions.remove(jobId);
    return true;
}

std::optional<Job> BackgroundJobService::findJob(const QString& jobId) const {
    auto it = m_jobs.find(jobId);
    if (it == m_jobs.end()) {
        return std::nullopt;
    }
    return it.value();
}

QList<Job> BackgroundJobService::jobs() const {
    return m_jobs.values();
}

QList<Job> BackgroundJobService::activeJobs() const {
    QList<Job> result;
    for (const auto& job : m_jobs) {
        if (job.state == JobState::Running || job.state == JobState::Queued) {
            result.append(job);
        }
    }
    return result;
}

QList<Job> BackgroundJobService::completedJobs() const {
    QList<Job> result;
    for (const auto& job : m_jobs) {
        if (job.state == JobState::Completed) {
            result.append(job);
        }
    }
    return result;
}

QList<Job> BackgroundJobService::failedJobs() const {
    QList<Job> result;
    for (const auto& job : m_jobs) {
        if (job.state == JobState::Failed) {
            result.append(job);
        }
    }
    return result;
}

void BackgroundJobService::setMaxConcurrentJobs(int maxJobs) {
    m_maxConcurrentJobs = maxJobs;
    m_threadPool.setMaxThreadCount(maxJobs);
}

int BackgroundJobService::maxConcurrentJobs() const {
    return m_maxConcurrentJobs;
}

bool BackgroundJobService::setPersistenceDatabase(QSqlDatabase database,
                                                  std::shared_ptr<StorageAdmission> admission) {
    if (!database.isValid() || !database.isOpen())
        return false;
    if (!m_jobs.isEmpty()) {
        return false;
    }
    admission_ = std::move(admission);
    generation_ = admission_ ? admission_->snapshot().generation : 0;
    m_database = std::move(database);
    return initializePersistence();
}

bool BackgroundJobService::initializePersistence() {
    auto writer = admission_ ? admission_->admit(generation_) : StorageAdmission::Writer{};
    if (admission_ && !writer) {
        return false;
    }
    if (!m_database.isOpen()) {
        return false;
    }
    QSqlQuery query(m_database);
    return query.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS sentinel_jobs ("
        "id TEXT PRIMARY KEY, name TEXT NOT NULL, description TEXT, state INTEGER NOT NULL, "
        "created_at TEXT, started_at TEXT, completed_at TEXT, error TEXT, retry_count INTEGER, "
        "max_retries INTEGER)"));
}

bool BackgroundJobService::persistJob(const Job& job) {
    auto writer = admission_ ? admission_->admit(generation_) : StorageAdmission::Writer{};
    if (admission_ && !writer) {
        return false;
    }
    if (!m_database.isValid()) {
        return true;
    }
    if (!m_database.isOpen()) {
        return false;
    }
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("INSERT OR REPLACE INTO sentinel_jobs "
                                 "(id,name,description,state,created_at,started_at,completed_at,"
                                 "error,retry_count,max_retries) "
                                 "VALUES(?,?,?,?,?,?,?,?,?,?)"));
    query.addBindValue(job.id);
    query.addBindValue(job.name);
    query.addBindValue(job.description);
    query.addBindValue(static_cast<int>(job.state));
    query.addBindValue(job.createdAt.toString(Qt::ISODateWithMs));
    query.addBindValue(job.startedAt.toString(Qt::ISODateWithMs));
    query.addBindValue(job.completedAt.toString(Qt::ISODateWithMs));
    query.addBindValue(job.errorString);
    query.addBindValue(job.retryCount);
    query.addBindValue(job.maxRetries);
    return query.exec();
}

int BackgroundJobService::recoverPersistedJobs() {
    if (!m_database.isOpen())
        return 0;
    QSqlQuery query(m_database);
    if (!query.exec(QStringLiteral("SELECT "
                                   "id,name,description,state,created_at,started_at,completed_at,"
                                   "error,retry_count,max_retries FROM sentinel_jobs"))) {
        return 0;
    }
    int recovered = 0;
    while (query.next()) {
        Job job;
        job.id = query.value(0).toString();
        job.name = query.value(1).toString();
        job.description = query.value(2).toString();
        job.state = static_cast<JobState>(query.value(3).toInt());
        job.createdAt = QDateTime::fromString(query.value(4).toString(), Qt::ISODateWithMs);
        job.startedAt = QDateTime::fromString(query.value(5).toString(), Qt::ISODateWithMs);
        job.completedAt = QDateTime::fromString(query.value(6).toString(), Qt::ISODateWithMs);
        job.errorString = query.value(7).toString();
        job.retryCount = query.value(8).toInt();
        job.maxRetries = query.value(9).toInt();
        if (job.state == JobState::Running || job.state == JobState::Queued) {
            job.state = JobState::Failed;
            job.errorString =
                QStringLiteral("Recovered after restart; execution function was not restored.");
            job.completedAt = QDateTime::currentDateTime();
        }
        m_jobs.insert(job.id, job);
        ++recovered;
    }
    return recovered;
}

void BackgroundJobService::processQueue() {
    QMutexLocker locker(&m_mutex);

    while (!m_jobQueue.isEmpty() && m_runningJobs < m_maxConcurrentJobs) {
        QString jobId = m_jobQueue.dequeue();
        auto it = m_jobs.find(jobId);
        if (it == m_jobs.end() || it->state != JobState::Queued) {
            continue;
        }

        m_runningJobs++;
        executeJob(jobId);
    }
}

void BackgroundJobService::executeJob(const QString& jobId) {
    auto jobIt = m_jobs.find(jobId);
    auto funcIt = m_jobFunctions.find(jobId);

    if (jobIt == m_jobs.end() || funcIt == m_jobFunctions.end()) {
        return;
    }

    jobIt->state = JobState::Running;
    jobIt->startedAt = QDateTime::currentDateTime();

    if (!persistJob(*jobIt)) {
        --m_runningJobs;
        jobIt->state = JobState::Failed;
        jobIt->errorString =
            QStringLiteral("Job start persistence refused; execution was not started.");
        operationLeases_.remove(jobId);
        emit jobFailed(jobId, jobIt->errorString);
        return;
    }
    const JobFunction func = funcIt.value();
    Job accepted = *jobIt;
    auto lifetime = operationLeases_.value(jobId);
    emit jobStarted(jobId);
    QPointer<BackgroundJobService> self(this);
    m_threadPool.start([self, jobId, func, accepted, lifetime]() mutable {
        auto progressCallback = [self, jobId](JobProgress progress) {
            if (!self) {
                return;
            }
            QMetaObject::invokeMethod(
                self.data(),
                [self, jobId, progress] {
                    if (!self) {
                        return;
                    }
                    QMutexLocker locker(&self->m_mutex);
                    auto it = self->m_jobs.find(jobId);
                    if (it != self->m_jobs.end()) {
                        it->progress = progress;
                        emit self->jobProgressUpdated(jobId, progress);
                    }
                },
                Qt::QueuedConnection);
        };
        bool success = false;
        try {
            success = func(accepted, progressCallback);
        } catch (const std::exception& e) {
            accepted.errorString = e.what();
        }
        if (!self) {
            return;
        }
        QMetaObject::invokeMethod(
            self.data(),
            [self, jobId, accepted, success, lifetime] {
                JobState finalState = JobState::Failed;
                QString finalError;
                {
                    if (!self) {
                        return;
                    }
                    QMutexLocker locker(&self->m_mutex);
                    --self->m_runningJobs;
                    auto it = self->m_jobs.find(jobId);
                    if (it == self->m_jobs.end())
                        return;
                    if (it->state != JobState::Cancelled) {
                        it->state = success ? JobState::Completed : JobState::Failed;
                        it->errorString = accepted.errorString;
                        it->completedAt = QDateTime::currentDateTime();
                    }
                    if (!self->persistJob(*it)) {
                        it->state = JobState::Failed;
                        it->errorString = QStringLiteral(
                            "Job result persistence refused; side effects may exist.");
                    }
                    self->operationLeases_.remove(jobId);
                    finalState = it->state;
                    finalError = it->errorString;
                }
                // Direct completion subscribers may submit another job; never call them
                // while holding the job mutex.
                if (finalState == JobState::Completed) {
                    emit self->jobCompleted(jobId);
                } else if (finalState == JobState::Failed) {
                    emit self->jobFailed(jobId, finalError);
                }
                if (self) {
                    self->processQueue();
                }
            },
            Qt::QueuedConnection);
    });
}

QString BackgroundJobService::generateJobId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

} // namespace sentinel::core
