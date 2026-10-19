// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/MemoryArchiveJobs.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFuture>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>
#include <QTemporaryFile>
#include <QUuid>
#include <QtConcurrentRun>
#include <atomic>
namespace sentinel::core {
struct MemoryArchiveJobs::Job {
    QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    MemoryScope scope = MemoryScope::Session;
    MemoryManagementContext context;
    MemoryArchiveLimits limits;
    std::unique_ptr<QTemporaryFile> file;
    QFuture<void> future;
    mutable QMutex mutex;
    std::atomic_bool cancel = false;
    QString state = "Uploading", error, wireDigest;
    qint64 expected = 0, offset = 0, processed = 0, processedBytes = 0;
    MemoryArchiveResult result;
    bool committing = false;
    QElapsedTimer touched;
};
MemoryArchiveJobs::MemoryArchiveJobs(QString path, QObject* owner, std::function<void()> restored)
    : databasePath_(std::move(path)), owner_(owner), restored_(std::move(restored)) {
    if (!QFileInfo(databasePath_).isFile()) {
        return;
    }
    directory_ = QFileInfo(databasePath_).dir().filePath("memory-archive-staging");
    QDir directory(directory_);
    const bool existed = directory.exists();
    if (!directory.mkpath(".") || QFileInfo(directory_).isSymLink() ||
        !QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                               QFileDevice::ExeOwner)) {
        return;
    }
    lock_ = std::make_unique<QLockFile>(directory.filePath("owner.lock"));
    lock_->setStaleLockTime(0);
    if (!lock_->tryLock(0)) {
        return;
    }
    QFile marker(directory.filePath("format"));
    if (existed) {
        if (!marker.open(QIODevice::ReadOnly) || marker.size() != 35 ||
            marker.read(36) != "sentinel-memory-archive-staging-v2\n") {
            return;
        }
    } else {
        if (!marker.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
            marker.write("sentinel-memory-archive-staging-v2\n") != 35) {
            return;
        }
        marker.close();
    }
    for (const auto& entry :
         directory.entryInfoList({"transfer-*.part"}, QDir::Files | QDir::NoSymLinks)) {
        if (!QFile::remove(entry.absoluteFilePath())) {
            return;
        }
    }
    available_ = true;
}
MemoryArchiveJobs::~MemoryArchiveJobs() {
    for (const auto& job : jobs_) {
        job->cancel = true;
    }
    for (const auto& job : jobs_) {
        job->future.waitForFinished();
    }
    jobs_.clear();
}
QJsonObject MemoryArchiveJobs::snapshot(const std::shared_ptr<Job>& job) const {
    QMutexLocker lock(&job->mutex);
    auto state = job->state;
    if (!job->future.isFinished()) {
        if (state == "Preflight") {
            state = "Validating";
        }
        if (state == "Exported") {
            state = "Exporting";
        }
        if (state == "Succeeded") {
            state = "Restoring";
        }
    }
    QJsonObject result{{"status", "Ready"},
                       {"job", job->id},
                       {"state", state},
                       {"detail", job->error},
                       {"size", job->expected},
                       {"offset", job->offset},
                       {"processed", job->processed},
                       {"processedBytes", job->processedBytes},
                       {"records", job->result.records},
                       {"version", job->result.version},
                       {"chunks", job->result.chunks},
                       {"sha256", job->wireDigest},
                       {"cancellable", !job->committing}};
    return result;
}
void MemoryArchiveJobs::run(const std::shared_ptr<Job>& job, const QString& operation, bool skip) {
    const auto path = databasePath_, fileName = job->file->fileName();
    const auto owner = owner_;
    const auto restored = restored_;
    job->future = QtConcurrent::run([job, path, fileName, operation, skip, owner, restored] {
        QElapsedTimer timer;
        timer.start();
        MemoryArchiveControl control;
        control.cancelled = [&] { return job->cancel.load() || timer.elapsed() > 30000; };
        control.progress = [&](qint64 records, qint64 bytes) {
            QMutexLocker lock(&job->mutex);
            job->processed = records;
            job->processedBytes = bytes;
        };
        control.beforeCommit = [&] {
            QMutexLocker lock(&job->mutex);
            if (control.cancelled()) {
                return false;
            }
            job->committing = true;
            return true;
        };
        QFile file(fileName);
        MemoryArchiveResult result;
        result.status = "ReadFailure";
        if (file.open(operation == "export" ? QIODevice::WriteOnly | QIODevice::Truncate
                                            : QIODevice::ReadOnly)) {
            if (operation == "export") {
                result = SQLiteMemoryStore::exportArchiveSnapshot(
                    path, file, job->scope, job->context, job->limits, control);
                if (!file.flush()) {
                    result.status = "WriteFailure";
                }
            } else if (operation == "validate") {
                result = readMemoryArchive(file, job->scope, job->context, job->limits, control);
            } else {
                SQLiteMemoryStore store(path);
                result = store.restoreArchive(file, job->scope, job->context, job->limits, control,
                                              skip);
            }
            file.close();
        }
        QString wireDigest;
        if (result.ok() && operation == "export") {
            if (!file.open(QIODevice::ReadOnly)) {
                result.status = "ReadFailure";
            } else {
                QCryptographicHash hash(QCryptographicHash::Sha256);
                while (!file.atEnd()) {
                    if (control.cancelled()) {
                        result.status = "Cancelled";
                        break;
                    }
                    const auto bytes = file.read(65536);
                    if (bytes.isEmpty() && !file.atEnd()) {
                        result.status = "ReadFailure";
                        break;
                    }
                    hash.addData(bytes);
                }
                wireDigest = QString::fromLatin1(hash.result().toHex());
            }
        }
        if (result.ok() && operation == "restore" && owner && restored) {
            QMetaObject::invokeMethod(owner, restored, Qt::QueuedConnection);
        }
        QMutexLocker lock(&job->mutex);
        job->result = result;
        job->wireDigest = std::move(wireDigest);
        if (result.ok()) {
            job->state = operation == "export"     ? "Exported"
                         : operation == "validate" ? "Preflight"
                                                   : "Succeeded";
            if (operation == "export") {
                job->expected = result.bytes;
            }
        } else {
            job->state = result.status == "Cancelled" ? "Cancelled" : "Failed";
            job->error = timer.elapsed() > 30000 ? "DeadlineExceeded" : result.status;
        }
    });
}
QJsonObject MemoryArchiveJobs::handle(const QJsonObject& input, MemoryScope scope,
                                      const MemoryManagementContext& context) {
    auto fail = [](const QString& status) { return QJsonObject{{"status", status}}; };
    if (!authorizedMemoryScope(scope, context)) {
        return fail("Unauthorized");
    }
    if (!available_) {
        return fail("Unavailable");
    }
    const auto operation = input.value("operation").toString();
    if (operation == "archiveExportBegin" || operation == "archiveImportBegin") {
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            auto job = it.value();
            QMutexLocker lock(&job->mutex);
            if (job->touched.hasExpired(600000)) {
                job->cancel = true;
            }
            if (job->future.isFinished() &&
                (job->touched.hasExpired(600000) || job->state == "Cancelled" ||
                 job->state == "Failed" || job->state == "Succeeded")) {
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
        if (jobs_.size() >= 2) {
            return fail("Busy");
        }
        auto job = std::make_shared<Job>();
        job->touched.start();
        job->scope = scope;
        job->context = context;
        if (input.contains("maxBytes")) {
            job->limits.maximumBytes = static_cast<qint64>(input.value("maxBytes").toDouble());
        }
        if (input.contains("maxRecords")) {
            job->limits.maximumRecords = input.value("maxRecords").toInt();
        }
        if (!job->limits.valid()) {
            return fail("LimitExceeded");
        }
        job->file =
            std::make_unique<QTemporaryFile>(QDir(directory_).filePath("transfer-XXXXXX.part"));
        if (!job->file->open() ||
            !job->file->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
            return fail("WriteFailure");
        }
        job->file->close();
        if (operation == "archiveImportBegin") {
            job->expected = static_cast<qint64>(input.value("size").toDouble(-1));
            if (job->expected < 1 || job->expected > job->limits.maximumBytes) {
                return fail("LimitExceeded");
            }
        } else {
            job->state = "Exporting";
        }
        jobs_[job->id] = job;
        if (operation == "archiveExportBegin") {
            run(job, "export");
        }
        return snapshot(job);
    }
    const auto job = jobs_.value(input.value("job").toString());
    if (!job) {
        return fail("NotFound");
    }
    job->touched.restart();
    if (job->scope != scope || job->context.access.sessionId != context.access.sessionId ||
        job->context.access.workspaceId != context.access.workspaceId ||
        job->context.access.includePersonal != context.access.includePersonal ||
        job->context.allowLegacy != context.allowLegacy) {
        return fail("Conflict");
    }
    if (operation == "archiveCancel") {
        {
            QMutexLocker lock(&job->mutex);
            if (!job->committing) {
                job->cancel = true;
                if (job->state == "Uploading" || job->state == "Preflight" ||
                    job->state == "Exported") {
                    job->state = "Cancelled";
                }
            }
        }
        return snapshot(job);
    }
    if (operation == "archivePoll") {
        return snapshot(job);
    }
    QString state;
    {
        QMutexLocker lock(&job->mutex);
        state = job->state;
    }
    if (operation == "archiveWrite" && state == "Uploading") {
        const auto encoded = input.value("data").toString().toLatin1();
        const auto decoded =
            QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (encoded.size() > 43692 || !decoded || decoded.decoded.isEmpty() ||
            decoded.decoded.size() > 32768 ||
            input.value("offset").toDouble(-1) != static_cast<double>(job->offset) ||
            job->offset + decoded.decoded.size() > job->expected) {
            return fail("InvalidInput");
        }
        QFile file(job->file->fileName());
        if (!file.open(QIODevice::WriteOnly | QIODevice::Append) ||
            file.write(decoded.decoded) != decoded.decoded.size() || !file.flush()) {
            return fail("WriteFailure");
        }
        job->offset += decoded.decoded.size();
        return snapshot(job);
    }
    if (operation == "archiveRead" && state == "Exported" && job->future.isFinished()) {
        const auto offset = static_cast<qint64>(input.value("offset").toDouble(-1));
        if (offset < 0 || offset >= job->expected) {
            return fail("InvalidInput");
        }
        QFile file(job->file->fileName());
        if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
            return fail("ReadFailure");
        }
        const auto bytes = file.read(32768);
        if (bytes.isEmpty()) {
            return fail("ReadFailure");
        }
        auto reply = snapshot(job);
        reply["data"] = QString::fromLatin1(bytes.toBase64());
        reply["offset"] = offset;
        reply["nextOffset"] = offset + bytes.size();
        return reply;
    }
    if (operation == "archiveValidate" && state == "Uploading" && job->offset == job->expected) {
        job->state = "Validating";
        run(job, "validate");
        return snapshot(job);
    }
    if (operation == "archiveCommit" && state == "Preflight" && job->future.isFinished()) {
        if (!input.value("confirmed").toBool() ||
            !QSet<QString>{"reject", "skipIdentical"}.contains(
                input.value("conflict").toString())) {
            return fail("InvalidInput");
        }
        job->state = "Restoring";
        run(job, "restore", input.value("conflict") == "skipIdentical");
        return snapshot(job);
    }
    return fail("Conflict");
}
} // namespace sentinel::core
