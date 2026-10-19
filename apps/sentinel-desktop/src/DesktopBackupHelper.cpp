// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/desktop/DesktopBackupHelper.h"
#include "sentinel/core/memory/MemoryManagement.h"
#include "sentinel/desktop/DesktopRuntimeClient.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryFile>
namespace sentinel::desktop {
namespace {
constexpr int maximumBytes = 16 * 1024 * 1024;
constexpr int chunkBytes = 48 * 1024;
const QStringList supported{"settings", "workspaceProfiles", "extensions", "chat", "memory"};
}
DesktopBackupHelper::DesktopBackupHelper(DesktopRuntimeClient* runtime, QObject* parent)
    : QObject(parent), runtime_(runtime) {
    if (!runtime_) return;
    connect(runtime_, &DesktopRuntimeClient::memoryManagementCompleted, this,
            [this](const QVariantMap& result) {
                if (memoryActive_ && result.value("requestTag").toString() == memoryTag_) {
                    receiveMemory(result);
                }
            });
    connect(&runtime_->transport(), &DaemonClient::responseReceived, this,
        [this](const QString& id, const QString& name, const QJsonObject& payload) {
            if (busy_ && id == request_ && name == "backup.transfer") receive(payload.value("result").toObject());
        });
    connect(&runtime_->transport(), &DaemonClient::requestFailed, this,
        [this](const QString& id, DaemonClient::Error, const QString&) {
            if (busy_ && id == request_) finish(tr("Backup transfer failed. Check the daemon connection and try again."));
        });
}
void DesktopBackupHelper::send(const QString& action, QJsonObject value) {
    action_ = action;
    emit changed();
    if (!transfer_.isEmpty()) value.insert("transferId", transfer_);
    request_ = runtime_->transport().request(DaemonClient::Command::backup_transfer, {{"action", action}, {"value", value}});
    if (request_.isEmpty()) finish(tr("The daemon is unavailable."));
}
void DesktopBackupHelper::finish(const QString& status) {
    busy_ = false; status_ = status; request_.clear(); bytes_.clear();
    if (runtime_ && !transfer_.isEmpty()) runtime_->transport().request(DaemonClient::Command::backup_transfer, {{"action", "cancel"}, {"value", QJsonObject{}}});
    transfer_.clear(); emit changed();
    if (runtime_) runtime_->refresh();
}
QVariantMap DesktopBackupHelper::loadMemoryArchive(const QUrl& path) {
    QFile file(path.toLocalFile());
    if (!path.isLocalFile() || !file.open(QIODevice::ReadOnly) || file.size() > 98304) {
        return {{"status", "Unavailable"}};
    }
    const auto bytes = file.read(98305);
    const auto parsed = core::decodeMemoryArchive(bytes);
    if (parsed.status != core::MemoryManagementStatus::Ready) {
        return {{"status", core::memoryManagementStatusName(parsed.status)}};
    }
    return {{"status", "Ready"},
            {"archive", QString::fromUtf8(bytes)},
            {"count", parsed.records.size()}};
}
bool DesktopBackupHelper::saveMemoryArchive(const QUrl& path, const QString& archive) {
    const auto bytes = archive.toUtf8();
    if (!path.isLocalFile() ||
        core::decodeMemoryArchive(bytes).status != core::MemoryManagementStatus::Ready) {
        return false;
    }
    QSaveFile file(path.toLocalFile());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}
bool DesktopBackupHelper::exportFile(const QUrl& path, const QStringList& domains) {
    if (busy_ || memoryActive_) {
        return false;
    }
    if (!runtime_ || !runtime_->ready() || !path.isLocalFile() || domains.isEmpty()) { finish(tr("Choose a destination and connect to the daemon.")); return false; }
    importing_ = false; destination_ = path; bytes_.clear(); transfer_.clear(); offset_ = 0; expected_ = 0;
    busy_ = true; status_ = tr("Preparing backup…"); emit changed();
    send("export", {{"domains", QJsonArray::fromStringList(domains)}}); return true;
}
bool DesktopBackupHelper::inspectFile(const QUrl& path) {
    if (busy_ || memoryActive_) {
        return false;
    }
    importDomains_.clear(); importBytes_.clear(); preview_.clear();
    QFile file(path.toLocalFile());
    if (!path.isLocalFile() || !file.open(QIODevice::ReadOnly) || file.size() <= 0 || file.size() > maximumBytes) { finish(tr("Choose a readable Sentinel JSON backup no larger than 16 MB.")); return false; }
    const auto bytes = file.read(maximumBytes + 1);
    const auto document = QJsonDocument::fromJson(bytes);
    const auto manifest = document.object().value("manifest").toObject();
    const auto data = document.object().value("data").toObject();
    if (bytes.size() > maximumBytes || !document.isObject() || manifest.value("formatVersion").toInt(-1) != 1 || !manifest.value("credentialsExcluded").toBool() || data.isEmpty()) { finish(tr("This file is not a supported Sentinel backup.")); return false; }
    QStringList domains;
    for (const auto& domain : manifest.value("includedDomains").toArray()) {
        if (!domain.isString() || !supported.contains(domain.toString()) || domains.contains(domain.toString()) || !data.contains(domain.toString())) { finish(tr("The backup manifest is invalid.")); return false; }
        domains.append(domain.toString());
    }
    if (domains.isEmpty()) { finish(tr("The backup contains no supported data.")); return false; }
    importDomains_ = domains; importBytes_ = bytes;
    preview_ = tr("%1 · %2 bytes · created %3\nContains: %4\nCredentials and model binaries are excluded. The daemon validates the complete file before applying it.")
        .arg(QFileInfo(file).fileName()).arg(bytes.size()).arg(manifest.value("createdAtUtc").toString(), domains.join(", "));
    status_ = tr("Backup inspected. Select the data to restore and confirm the import."); emit changed(); return true;
}
bool DesktopBackupHelper::importFile(const QStringList& domains, bool replace) {
    if (busy_ || memoryActive_) {
        return false;
    }
    if (!runtime_ || !runtime_->ready() || importBytes_.isEmpty() || domains.isEmpty()) { finish(tr("Inspect a backup and connect to the daemon first.")); return false; }
    for (const auto& domain : domains) if (!importDomains_.contains(domain)) { finish(tr("Select only data present in this backup.")); return false; }
    importing_ = true; bytes_ = importBytes_; transfer_.clear(); offset_ = 0; expected_ = bytes_.size();
    digest_ = QCryptographicHash::hash(bytes_, QCryptographicHash::Sha256).toHex();
    busy_ = true; status_ = tr("Uploading and validating backup…"); emit changed();
    send("importBegin", {{"domains", QJsonArray::fromStringList(domains)}, {"size", expected_}, {"sha256", QString::fromLatin1(digest_)}, {"replace", replace}}); return true;
}
void DesktopBackupHelper::cancel() {
    if (!cancellable()) {
        return;
    }
    if (memoryActive_) {
        if (memoryJob_.isEmpty()) {
            memoryCancelPending_ = true;
            status_ = tr("Archive cancellation requested; waiting for the job identity.");
            emit changed();
        } else {
            sendMemory("archiveCancel");
        }
    } else {
        finish(tr("Backup transfer cancelled."));
    }
}
void DesktopBackupHelper::receive(const QJsonObject& result) {
    if (!result.value("succeeded").toBool()) { finish(tr("The backup operation was rejected (%1). Existing data has not been reported as successfully changed.").arg(result.value("code").toString(result.value("detail").toString()))); return; }
    if (action_ == "export" || action_ == "importBegin") {
        transfer_ = result.value("transferId").toString();
        const int size = result.value("size").toInt(-1);
        if (transfer_.isEmpty() || size <= 0 || size > maximumBytes || (importing_ && size != expected_)) { finish(tr("Invalid backup transfer response.")); return; }
        expected_ = size;
        if (!importing_) digest_ = result.value("sha256").toString().toLatin1();
    } else if (action_ == "read") {
        const auto decoded = QByteArray::fromBase64Encoding(result.value("data").toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > chunkBytes || result.value("offset").toInt(-1) != offset_ || offset_ + decoded.decoded.size() > expected_ || result.value("nextOffset").toInt(-1) != offset_ + decoded.decoded.size()) { finish(tr("Invalid backup chunk.")); return; }
        bytes_.append(decoded.decoded); offset_ = bytes_.size();
    } else if (action_ == "write") {
        const int next = result.value("nextOffset").toInt(-1);
        if (next != qMin(offset_ + chunkBytes, expected_)) { finish(tr("Invalid backup acknowledgement.")); return; }
        offset_ = next;
    } else if (action_ == "commit") { finish(tr("Backup restored successfully.")); return; }
    emit changed();
    if (offset_ < expected_) {
        if (importing_) send("write", {{"offset", offset_}, {"data", QString::fromLatin1(bytes_.mid(offset_, chunkBytes).toBase64())}});
        else send("read", {{"offset", offset_}});
    } else if (importing_) send("commit");
    else {
        if (QCryptographicHash::hash(bytes_, QCryptographicHash::Sha256).toHex() != digest_) { finish(tr("Backup integrity verification failed.")); return; }
        QSaveFile file(destination_.toLocalFile());
        if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || file.write(bytes_) != bytes_.size() || !file.commit()) { finish(tr("Could not save the backup. Check the destination and free space.")); return; }
        finish(tr("Backup saved successfully."));
    }
}
}
