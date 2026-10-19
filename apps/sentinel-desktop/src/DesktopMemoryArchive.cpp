// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/desktop/DesktopBackupHelper.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QTimer>
#include <QUuid>
namespace sentinel::desktop {
DesktopBackupHelper::~DesktopBackupHelper() {
    if (memoryActive_ && runtime_ && !memoryJob_.isEmpty()) {
        sendMemory("archiveCancel");
    }
}
void DesktopBackupHelper::sendMemory(const QString& operation, QJsonObject extra) {
    auto request = QJsonObject::fromVariantMap(memoryRequest_);
    request["operation"] = operation;
    request["job"] = memoryJob_;
    memoryTag_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    request["requestTag"] = memoryTag_;
    for (auto it = extra.begin(); it != extra.end(); ++it) {
        request[it.key()] = it.value();
    }
    runtime_->dispatch("manageMemory", {request.toVariantMap()});
}
void DesktopBackupHelper::pollMemory() {
    const auto job = memoryJob_;
    QTimer::singleShot(100, this, [this, job] {
        if (memoryActive_ && busy_ && memoryJob_ == job && !job.isEmpty()) {
            sendMemory("archivePoll");
        }
    });
}
void DesktopBackupHelper::finishMemory(const QString& text) {
    if (runtime_ && runtime_->ready() && !memoryJob_.isEmpty()) {
        auto cleanup = memoryRequest_;
        cleanup["operation"] = "archiveCancel";
        cleanup["job"] = memoryJob_;
        cleanup["requestTag"] = "cleanup-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        runtime_->dispatch("manageMemory", {cleanup});
    }
    busy_ = false;
    memoryReady_ = false;
    memoryActive_ = false;
    status_ = text;
    memorySource_.reset();
    memoryTarget_.reset();
    memoryHash_.reset();
    memoryJob_.clear();
    emit changed();
}
bool DesktopBackupHelper::exportMemoryFile(const QUrl& url, const QVariantMap& request) {
    if (busy_ || memoryReady_ || !runtime_ || !runtime_->ready() || !url.isLocalFile()) {
        return false;
    }
    const QFileInfo destination(url.toLocalFile());
    if (destination.exists() || destination.isSymLink() || destination.fileName().isEmpty() ||
        !destination.dir().exists()) {
        status_ = tr("Choose a new archive filename; existing files are never overwritten.");
        emit changed();
        return false;
    }
    memoryTarget_ = std::make_unique<QTemporaryFile>(
        destination.dir().filePath(".sentinel-memory-XXXXXX.part"));
    if (!memoryTarget_->open() ||
        !memoryTarget_->setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        finishMemory(tr("Cannot create a private archive staging file."));
        return false;
    }
    memoryActive_ = true;
    memoryReady_ = false;
    memoryCanCancel_ = true;
    memoryCancelPending_ = false;
    busy_ = true;
    memoryMode_ = "export";
    memoryRequest_ = request;
    memoryJob_.clear();
    destination_ = url;
    offset_ = 0;
    expected_ = 0;
    memoryHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    status_ = tr("Creating a consistent memory snapshot…");
    emit changed();
    sendMemory("archiveExportBegin");
    return true;
}
bool DesktopBackupHelper::inspectMemoryFile(const QUrl& url, const QVariantMap& request) {
    if (busy_ || memoryReady_ || !runtime_ || !runtime_->ready() || !url.isLocalFile()) {
        return false;
    }
    const QFileInfo info(url.toLocalFile());
    const auto limit = request.value("maxBytes", 64 * 1024 * 1024).toLongLong();
    if (!info.isFile() || info.isSymLink() || info.size() < 1 || info.size() > limit ||
        limit > 256LL * 1024 * 1024) {
        status_ = tr("Choose a regular archive within the configured byte limit.");
        emit changed();
        return false;
    }
    memorySource_ = std::make_unique<QFile>(info.absoluteFilePath());
    if (!memorySource_->open(QIODevice::ReadOnly)) {
        finishMemory(tr("The archive cannot be read."));
        return false;
    }
    memoryActive_ = true;
    memoryReady_ = false;
    memoryCanCancel_ = true;
    memoryCancelPending_ = false;
    busy_ = true;
    memoryMode_ = "import";
    memoryRequest_ = request;
    memoryJob_.clear();
    offset_ = 0;
    expected_ = static_cast<int>(info.size());
    preview_.clear();
    status_ = tr("Uploading archive for authorized preflight…");
    emit changed();
    sendMemory("archiveImportBegin", {{"size", expected_}});
    return true;
}
bool DesktopBackupHelper::restoreMemoryFile(bool skipIdentical) {
    if (!memoryActive_ || !memoryReady_ || busy_ || !runtime_ || !runtime_->ready()) {
        return false;
    }
    memoryReady_ = false;
    busy_ = true;
    status_ = tr("Restoring atomically; cancellation is available until commit starts…");
    emit changed();
    sendMemory("archiveCommit",
               {{"confirmed", true}, {"conflict", skipIdentical ? "skipIdentical" : "reject"}});
    return true;
}
void DesktopBackupHelper::receiveMemory(const QVariantMap& result) {
    if (result.value("status").toString() != "Ready") {
        finishMemory(tr("Archive operation failed (%1). Restore outcome after transport loss may "
                        "be unknown; inspect the scope before retrying.")
                         .arg(result.value("status").toString()));
        return;
    }
    memoryCanCancel_ = result.value("cancellable", true).toBool();
    const auto state = result.value("state").toString();
    if (memoryJob_.isEmpty()) {
        memoryJob_ = result.value("job").toString();
    }
    if (memoryJob_.isEmpty() || memoryJob_ != result.value("job").toString()) {
        finishMemory(tr("Invalid archive job identity."));
        return;
    }
    if (memoryCancelPending_) {
        memoryCancelPending_ = false;
        sendMemory("archiveCancel");
        return;
    }
    if (state == "Failed") {
        finishMemory(tr("Archive failed: %1. No incomplete restore is reported as successful.")
                         .arg(result.value("detail").toString()));
        return;
    }
    if (state == "Cancelled") {
        finishMemory(tr("Archive cancelled. Uncommitted restore changes were rolled back."));
        return;
    }
    if (state == "Succeeded") {
        finishMemory(tr("Memory archive restored successfully."));
        return;
    }
    if (state == "Preflight") {
        memorySource_.reset();
        busy_ = false;
        memoryReady_ = true;
        preview_ = tr("Archive v%1 · %2 records · %3 chunks · %4 bytes. Scope/owners and integrity "
                      "validated. Hashes provide integrity, not authenticity or encryption. "
                      "Conflicts are never overwritten.")
                       .arg(result.value("version").toInt())
                       .arg(result.value("records").toLongLong())
                       .arg(result.value("chunks").toInt())
                       .arg(expected_);
        status_ = tr("Preflight complete. Confirm the import or cancel.");
        emit changed();
        return;
    }
    if (state == "Uploading") {
        const int acknowledged = result.value("offset", -1).toInt();
        if (acknowledged != offset_) {
            finishMemory(tr("Invalid archive upload offset."));
            return;
        }
        if (offset_ == expected_) {
            sendMemory("archiveValidate");
            return;
        }
        const auto bytes = memorySource_->read(qMin(32768, expected_ - offset_));
        if (bytes.isEmpty()) {
            sendMemory("archiveCancel");
            return;
        }
        const int position = offset_;
        offset_ += static_cast<int>(bytes.size());
        emit changed();
        sendMemory("archiveWrite",
                   {{"offset", position}, {"data", QString::fromLatin1(bytes.toBase64())}});
        return;
    }
    if (state == "Exported") {
        if (expected_ == 0) {
            expected_ = result.value("size", -1).toInt();
            digest_ = result.value("sha256").toString().toLatin1();
        }
        if (expected_ < 1 ||
            expected_ > memoryRequest_.value("maxBytes", 64 * 1024 * 1024).toInt()) {
            finishMemory(tr("Invalid archive size."));
            return;
        }
        if (result.contains("data")) {
            const auto decoded =
                QByteArray::fromBase64Encoding(result.value("data").toString().toLatin1(),
                                               QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > 32768 ||
                result.value("offset", -1).toInt() != offset_ ||
                result.value("nextOffset", -1).toInt() != offset_ + decoded.decoded.size() ||
                offset_ + decoded.decoded.size() > expected_ ||
                memoryTarget_->write(decoded.decoded) != decoded.decoded.size()) {
                finishMemory(
                    tr("Archive chunk or disk write failed; no completed file was published."));
                return;
            }
            memoryHash_->addData(decoded.decoded);
            offset_ += static_cast<int>(decoded.decoded.size());
            emit changed();
        }
        if (offset_ < expected_) {
            sendMemory("archiveRead", {{"offset", offset_}});
            return;
        }
        if (memoryHash_->result().toHex() != digest_ || !memoryTarget_->flush()) {
            finishMemory(tr("Archive integrity or disk flush failed."));
            return;
        }
        memoryTarget_->close();
        if (QFileInfo::exists(destination_.toLocalFile()) ||
            !memoryTarget_->rename(destination_.toLocalFile())) {
            finishMemory(tr("Destination exists or atomic publication failed; no unrelated file "
                            "was overwritten."));
            return;
        }
        memoryTarget_->setAutoRemove(false);
        finishMemory(tr("Memory scope archive saved successfully."));
        return;
    }
    status_ = tr("%1 · %2 records processed. Transfer %3/%4 bytes; cancellation remains explicit.")
                  .arg(state)
                  .arg(result.value("processed").toLongLong())
                  .arg(offset_)
                  .arg(expected_);
    emit changed();
    pollMemory();
}
} // namespace sentinel::desktop
