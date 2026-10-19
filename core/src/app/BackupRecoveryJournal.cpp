// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/BackupRecoveryJournal.h"
#include "sentinel/core/platform/RecoveryJournalFileSecurity.h"
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QScopeGuard>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QUuid>
#include <cmath>
namespace sentinel::core {
namespace {
constexpr qint64 maximumDomainBytes = 256LL * 1024 * 1024;
constexpr int chunkBytes = 64 * 1024;
constexpr auto recoverySchema =
    ("CREATE TABLE recovery(id INTEGER PRIMARY KEY CHECK(id=1),payload BLOB NOT "
     "NULL,sha256 BLOB NOT NULL)");
constexpr auto preimageSchema =
    ("CREATE TABLE preimages(domain INTEGER NOT NULL,chunk INTEGER NOT NULL,payload "
     "BLOB NOT NULL,PRIMARY KEY(domain,chunk))");
bool uuid(const QString& s) {
    const QUuid id(s);
    return !id.isNull() && id.toString(QUuid::WithoutBraces) == s;
}
bool digest(const QByteArray& s) {
    if (s.size() != 64) {
        return false;
    }
    for (const auto c : s) {
        if ((c < '0' || c > '9') && (c < 'a' || c > 'f')) {
            return false;
        }
    }
    return true;
}
QByteArray hash(const QByteArray& s) {
    return QCryptographicHash::hash(s, QCryptographicHash::Sha256).toHex();
}
using recoveryfs::privatePath;
using recoveryfs::syncDirectory;
bool supportedSchema(int domain, int version) {
    return (domain == 1 && version == 7) || (domain == 2 && version == 6);
}
QString streamDigest(QIODevice* source, QByteArray* output, qint64* count) {
    if (!source || !source->isOpen() || !source->isReadable() || source->isSequential() ||
        source->pos() != 0) {
        return "InvalidInput";
    }
    QCryptographicHash h(QCryptographicHash::Sha256);
    *count = 0;
    QElapsedTimer timer;
    timer.start();
    while (!source->atEnd()) {
        if (timer.elapsed() > 30000) {
            return "TimedOut";
        }
        const auto bytes = source->read(chunkBytes);
        if (bytes.isEmpty() || bytes.size() > maximumDomainBytes - *count) {
            return "ReadFailure";
        }
        *count += bytes.size();
        h.addData(bytes);
    }
    *output = h.result().toHex();
    return "Ready";
}
} // namespace
QString backupRecoveryStateName(BackupRecoveryState s) {
    switch (s) {
    case BackupRecoveryState::Preflight:
        return "Preflight";
    case BackupRecoveryState::Validated:
        return "Validated";
    case BackupRecoveryState::Prepared:
        return "Prepared";
    case BackupRecoveryState::Applying:
        return "Applying";
    case BackupRecoveryState::Committed:
        return "Committed";
    case BackupRecoveryState::RolledBack:
        return "RolledBack";
    case BackupRecoveryState::RecoveryRequired:
        return "RecoveryRequired";
    }
    return "RecoveryRequired";
}
struct BackupRecoveryJournal::Impl {
    QString profileDirectory, profileId, root, path, connection, operation, archive;
    QSqlDatabase db;
    QThread* thread = QThread::currentThread();
    std::unique_ptr<QLockFile> lock;
    QJsonArray domains;
    BackupRecoveryState state = BackupRecoveryState::Preflight;
    int sequence = 0;
    quint64 device = 0, inode = 0, rootDevice = 0, rootInode = 0;
    bool opened = false, present = false, allowEmpty = false;
    bool identity() const {
        quint64 dev = 0, ino = 0, rdev = 0, rino = 0;
        return QThread::currentThread() == thread && opened &&
               privatePath(root, true, &rdev, &rino) && rdev == rootDevice && rino == rootInode &&
               privatePath(path, false, &dev, &ino) && dev == device && ino == inode;
    }
    QByteArray payload(BackupRecoveryState value, int seq, const QJsonArray& inventory) const {
        return QJsonDocument(QJsonObject{{"format", "sentinel-backup-recovery"},
                                         {"version", 1},
                                         {"profile", profileId},
                                         {"operation", operation},
                                         {"archive", archive},
                                         {"state", static_cast<int>(value)},
                                         {"sequence", seq},
                                         {"domains", inventory}})
            .toJson(QJsonDocument::Compact);
    }
    QString load() {
        if (!identity()) {
            return "InvalidJournal";
        }
        QSqlQuery q(db);
        if (!q.exec("SELECT CASE WHEN length(payload)<=8192 THEN payload END, CASE WHEN "
                    "length(sha256)=64 THEN sha256 END FROM recovery WHERE id=1")) {
            return "InvalidJournal";
        }
        if (!q.next()) {
            present = false;
            return allowEmpty ? "Ready" : "InvalidJournal";
        }
        const auto bytes = q.value(0).toByteArray();
        if (bytes.size() > 8192 || hash(bytes) != q.value(1).toByteArray()) {
            return "InvalidJournal";
        }
        const auto o = QJsonDocument::fromJson(bytes).object();
        const auto st = o.value("state").toInt(-1), seq = o.value("sequence").toInt(-1);
        const auto inventory = o.value("domains").toArray();
        if (o.size() != 8 || o.value("format") != "sentinel-backup-recovery" ||
            o.value("version") != 1 || o.value("profile").toString() != profileId ||
            !uuid(o.value("operation").toString()) ||
            !digest(o.value("archive").toString().toLatin1()) || st < 0 || st > 6 || seq < 1 ||
            seq > 16 || !o.value("domains").isArray() || inventory.size() > 2 || q.next()) {
            return "InvalidJournal";
        }
        int previous = 0;
        for (const auto& entry : inventory) {
            const auto item = entry.toObject();
            const int domain = item.value("domain").toInt(-1);
            const auto size = item.value("bytes").toDouble(-1);
            if (item.size() != 5 || domain <= previous || domain > 2 ||
                item.value("domain").toDouble() != domain ||
                item.value("schema").toDouble() != item.value("schema").toInt(-1) ||
                !supportedSchema(domain, item.value("schema").toInt(-1)) || size < 0 ||
                size > maximumDomainBytes || size != std::trunc(size) ||
                !digest(item.value("before").toString().toLatin1()) ||
                !digest(item.value("after").toString().toLatin1())) {
                return "InvalidJournal";
            }
            previous = domain;
        }
        if ((st >= 2 && st != 5 && inventory.isEmpty()) || (st < 2 && !inventory.isEmpty())) {
            return "InvalidJournal";
        }
        operation = o.value("operation").toString();
        archive = o.value("archive").toString();
        state = static_cast<BackupRecoveryState>(st);
        sequence = seq;
        domains = inventory;
        present = true;
        allowEmpty = false;
        // Require canonical numeric encodings and exact field types, not Qt coercions.
        if (bytes != payload(state, sequence, domains)) {
            return "InvalidJournal";
        }
        QSqlQuery storedDomains(db);
        if (!storedDomains.exec("SELECT DISTINCT domain FROM preimages ORDER BY domain")) {
            return "InvalidJournal";
        }
        QList<int> expectedDomains;
        for (const auto& item : domains) {
            if (item.toObject().value("bytes").toInteger(-1) > 0) {
                expectedDomains.append(item.toObject().value("domain").toInt());
            }
        }
        int index = 0;
        while (storedDomains.next()) {
            if (index >= expectedDomains.size() ||
                storedDomains.value(0).toInt() != expectedDomains.at(index)) {
                return "InvalidJournal";
            }
            ++index;
        }
        if (storedDomains.lastError().isValid() || index != expectedDomains.size()) {
            return "InvalidJournal";
        }
        return "Ready";
    }
    QString save(BackupRecoveryState next, const QJsonArray& inventory) {
        const auto bytes = payload(next, sequence + 1, inventory);
        QSqlQuery q(db);
        q.prepare("INSERT INTO recovery(id,payload,sha256) VALUES(1,?,?) ON CONFLICT(id) DO UPDATE "
                  "SET payload=excluded.payload,sha256=excluded.sha256");
        q.addBindValue(bytes);
        q.addBindValue(hash(bytes));
        if (!q.exec()) {
            return "WriteFailure";
        }
        return "Ready";
    }
    QString transition(BackupRecoveryState next) {
        if (load() != "Ready" || !present) {
            return "InvalidJournal";
        }
        if (!db.transaction()) {
            return "WriteFailure";
        }
        auto rollback = qScopeGuard([&] { db.rollback(); });
        if (save(next, domains) != "Ready" || !db.commit()) {
            return "WriteFailure";
        }
        rollback.dismiss();
        return load();
    }
    QString preimage(int domain, QIODevice* destination, QByteArray* checksum, qint64* size) {
        QCryptographicHash h(QCryptographicHash::Sha256);
        QSqlQuery q(db);
        q.setForwardOnly(true);
        q.prepare("SELECT CASE WHEN typeof(chunk)='integer' THEN chunk ELSE -1 END,CASE WHEN "
                  "length(payload)<=65536 THEN payload END FROM preimages "
                  "WHERE domain=? ORDER BY chunk");
        q.addBindValue(domain);
        if (!q.exec()) {
            return "InvalidJournal";
        }
        int index = 0;
        QElapsedTimer timer;
        timer.start();
        *size = 0;
        while (q.next()) {
            if (index >= maximumDomainBytes / chunkBytes || timer.elapsed() > 30000) {
                return "LimitExceeded";
            }
            const auto bytes = q.value(1).toByteArray();
            if (q.value(0).toInt() != index++ || bytes.isEmpty() || bytes.size() > chunkBytes ||
                bytes.size() > maximumDomainBytes - *size) {
                return "InvalidJournal";
            }
            h.addData(bytes);
            *size += bytes.size();
            if (destination && destination->write(bytes) != bytes.size()) {
                return "WriteFailure";
            }
        }
        if (q.lastError().isValid()) {
            return "ReadFailure";
        }
        *checksum = h.result().toHex();
        return "Ready";
    }
};
BackupRecoveryJournal::BackupRecoveryJournal(QString directory, QString id)
    : d_(std::make_unique<Impl>()) {
    d_->profileDirectory = std::move(directory);
    d_->profileId = std::move(id);
}
BackupRecoveryJournal::~BackupRecoveryJournal() {
    if (d_->db.isValid()) {
        d_->db.close();
        d_->db = {};
        QSqlDatabase::removeDatabase(d_->connection);
    }
}
QString BackupRecoveryJournal::open() {
#if !defined(Q_OS_UNIX)
    return "UnsupportedPlatform";
#else
    auto& d = *d_;
    if (QThread::currentThread() != d.thread) {
        return "WrongThread";
    }
    if (d.opened) {
        return d.load();
    }
    if (d.lock || d.db.isValid()) {
        return "Unavailable";
    }
    const QFileInfo parent(d.profileDirectory);
    if (!uuid(d.profileId) || !parent.isAbsolute() || !parent.isDir() || parent.isSymLink() ||
        parent.canonicalFilePath() != parent.absoluteFilePath() ||
        !recoveryfs::trustedProfileDirectory(d.profileDirectory)) {
        return "InvalidInput";
    }
    d.root = QDir(d.profileDirectory).filePath("backup-recovery");
    d.path = QDir(d.root).filePath("journal.sqlite3");
    if (!QFileInfo::exists(d.root)) {
        if (!recoveryfs::createPrivateDirectory(d.root) || !syncDirectory(d.profileDirectory)) {
            return "WriteFailure";
        }
    }
    if (!privatePath(d.root, true, &d.rootDevice, &d.rootInode)) {
        return "InvalidJournal";
    }
    const QFileInfo lockInfo(QDir(d.root).filePath("owner.lock"));
    if (lockInfo.isSymLink() || (lockInfo.exists() && !lockInfo.isFile())) {
        return "InvalidJournal";
    }
    d.lock = std::make_unique<QLockFile>(QDir(d.root).filePath("owner.lock"));
    d.lock->setStaleLockTime(0);
    if (!d.lock->tryLock(0)) {
        return "Busy";
    }
    const bool existed = QFileInfo::exists(d.path);
    if (!existed) {
        if (!recoveryfs::createPrivateFile(d.path)) {
            return "WriteFailure";
        }
    }
    if (!privatePath(d.path, false, &d.device, &d.inode) ||
        QFileInfo(d.path).size() > 600LL * 1024 * 1024) {
        return "InvalidJournal";
    }
    d.connection = "sentinel-backup-journal-" + QUuid::createUuid().toString(QUuid::Id128);
    d.db = QSqlDatabase::addDatabase("QSQLITE", d.connection);
    d.db.setDatabaseName(d.path);
    d.db.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
    if (!d.db.open()) {
        return "Unavailable";
    }
    QSqlQuery q(d.db);
    for (const auto* sql :
         {"PRAGMA trusted_schema=OFF", "PRAGMA synchronous=EXTRA", "PRAGMA fullfsync=ON"}) {
        if (!q.exec(QString::fromLatin1(sql))) {
            return "Unavailable";
        }
    }
    if (!q.exec("PRAGMA synchronous") || !q.next() || q.value(0).toInt() != 3 ||
        !q.exec("PRAGMA fullfsync") || !q.next() || q.value(0).toInt() != 1) {
        return "Unavailable";
    }
    if (!existed) {
        if (!d.db.transaction() || !q.exec(recoverySchema) || !q.exec(preimageSchema) ||
            !q.exec("PRAGMA application_id=1396855370") || !q.exec("PRAGMA user_version=1") ||
            !d.db.commit() || !syncDirectory(d.root)) {
            return "WriteFailure";
        }
    }
    if (!q.exec("PRAGMA application_id") || !q.next() || q.value(0).toInt() != 1396855370 ||
        !q.exec("PRAGMA user_version") || !q.next() || q.value(0).toInt() != 1) {
        return "InvalidJournal";
    }
    if (!q.exec("PRAGMA journal_mode") || !q.next() || q.value(0).toString() != "delete") {
        return "InvalidJournal";
    }
    if (!q.exec("SELECT name FROM sqlite_master WHERE type IN ('trigger','view')") || q.next()) {
        return "InvalidJournal";
    }
    if (!q.exec("SELECT name,sql FROM sqlite_master WHERE type='table' ORDER BY name") ||
        !q.next() || q.value(0).toString() != "preimages" ||
        q.value(1).toString() != preimageSchema || !q.next() ||
        q.value(0).toString() != "recovery" || q.value(1).toString() != recoverySchema ||
        q.next()) {
        return "InvalidJournal";
    }
    d.allowEmpty = !existed;
    d.opened = true;
    auto status = d.load();
    // Empty databases from another profile are not accepted as owned journals.
    if (existed && !d.present) {
        return "InvalidJournal";
    }
    return status;
#endif
}
QString BackupRecoveryJournal::begin(const QString& operationId, const QByteArray& archiveSha256) {
    auto& d = *d_;
    if (d.load() != "Ready" || d.present || !uuid(operationId) || !digest(archiveSha256)) {
        return "InvalidInput";
    }
    d.operation = operationId;
    d.archive = QString::fromLatin1(archiveSha256);
    if (!d.db.transaction()) {
        return "WriteFailure";
    }
    auto rollback = qScopeGuard([&] { d.db.rollback(); });
    if (d.save(BackupRecoveryState::Preflight, {}) != "Ready" || !d.db.commit()) {
        return "WriteFailure";
    }
    rollback.dismiss();
    return d.load();
}
QString BackupRecoveryJournal::validated() {
    if (d_->load() != "Ready" || !d_->present || d_->state != BackupRecoveryState::Preflight) {
        return "InvalidState";
    }
    return d_->transition(BackupRecoveryState::Validated);
}
QString BackupRecoveryJournal::prepare(const QList<BackupRecoveryPreimage>& inputs,
                                       const std::function<bool()>& cancelled) {
    auto& d = *d_;
    if (d.load() != "Ready" || !d.present || d.state != BackupRecoveryState::Validated ||
        inputs.isEmpty() || inputs.size() > 2) {
        return "InvalidState";
    }
    if (!d.db.transaction()) {
        return "WriteFailure";
    }
    auto rollback = qScopeGuard([&] { d.db.rollback(); });
    QJsonArray inventory;
    QElapsedTimer timer;
    timer.start();
    int previous = 0;
    for (const auto& input : inputs) {
        const int domain = static_cast<int>(input.domain);
        if (domain <= previous || domain > 2 || !supportedSchema(domain, input.schemaVersion) ||
            !digest(input.expectedRestoredSha256) || !input.source ||
            input.source->isSequential() || !input.source->isReadable() ||
            input.source->pos() != 0) {
            return "InvalidInput";
        }
        previous = domain;
        QCryptographicHash h(QCryptographicHash::Sha256);
        qint64 size = 0;
        int chunk = 0;
        while (!input.source->atEnd()) {
            if (chunk >= maximumDomainBytes / chunkBytes || timer.elapsed() > 30000) {
                return "LimitExceeded";
            }
            if (cancelled && cancelled()) {
                return "Cancelled";
            }
            const auto bytes = input.source->read(chunkBytes);
            if (bytes.isEmpty() || bytes.size() > maximumDomainBytes - size) {
                return "ReadFailure";
            }
            size += bytes.size();
            h.addData(bytes);
            QSqlQuery insert(d.db);
            insert.prepare("INSERT INTO preimages(domain,chunk,payload) VALUES(?,?,?)");
            insert.addBindValue(domain);
            insert.addBindValue(chunk++);
            insert.addBindValue(bytes);
            if (!insert.exec()) {
                return "WriteFailure";
            }
        }
        const auto before = h.result().toHex();
        QByteArray verified;
        qint64 verifiedBytes = 0;
        if (d.preimage(domain, nullptr, &verified, &verifiedBytes) != "Ready" ||
            before != verified || size != verifiedBytes) {
            return "IntegrityFailure";
        }
        inventory.append(QJsonObject{{"domain", domain},
                                     {"schema", input.schemaVersion},
                                     {"bytes", size},
                                     {"before", QString::fromLatin1(before)},
                                     {"after", QString::fromLatin1(input.expectedRestoredSha256)}});
    }
    if (cancelled && cancelled()) {
        return "Cancelled";
    }
    if (d.save(BackupRecoveryState::Prepared, inventory) != "Ready" || !d.db.commit()) {
        return "WriteFailure";
    }
    rollback.dismiss();
    return d.load();
}
QString BackupRecoveryJournal::applying() {
    if (d_->load() != "Ready" || !d_->present || d_->state != BackupRecoveryState::Prepared) {
        return "InvalidState";
    }
    // Recheck persisted preimages before granting the caller an Applying receipt.
    for (const auto& entry : d_->domains) {
        const auto item = entry.toObject();
        QByteArray sum;
        qint64 size = 0;
        if (d_->preimage(item.value("domain").toInt(), nullptr, &sum, &size) != "Ready" ||
            sum != item.value("before").toString().toLatin1() ||
            size != item.value("bytes").toInteger(-1)) {
            return "IntegrityFailure";
        }
    }
    return d_->transition(BackupRecoveryState::Applying);
}
QString BackupRecoveryJournal::recoveryRequired() {
    if (d_->load() != "Ready" || !d_->present ||
        (d_->state != BackupRecoveryState::Applying &&
         d_->state != BackupRecoveryState::Prepared)) {
        return "InvalidState";
    }
    return d_->transition(BackupRecoveryState::RecoveryRequired);
}
QString BackupRecoveryJournal::finish(bool rollback,
                                      const QList<BackupRecoveryVerification>& readers) {
    auto& d = *d_;
    if (d.load() != "Ready" || !d.present) {
        return "InvalidJournal";
    }
    if (d.state == BackupRecoveryState::Committed || d.state == BackupRecoveryState::RolledBack) {
        return "InvalidState";
    }
    if (!rollback && d.state != BackupRecoveryState::Applying) {
        return "InvalidState";
    }
    if (readers.size() != d.domains.size()) {
        return "IntegrityFailure";
    }
    for (int i = 0; i < readers.size(); ++i) {
        const auto item = d.domains.at(i).toObject();
        QByteArray sum;
        qint64 size = 0;
        if (static_cast<int>(readers.at(i).domain) != item.value("domain").toInt() ||
            streamDigest(readers.at(i).source, &sum, &size) != "Ready" ||
            sum != item.value(rollback ? "before" : "after").toString().toLatin1() ||
            (rollback && size != item.value("bytes").toInteger(-1))) {
            return "IntegrityFailure";
        }
    }
    return d.transition(rollback ? BackupRecoveryState::RolledBack
                                 : BackupRecoveryState::Committed);
}
QJsonObject BackupRecoveryJournal::inspect() {
    const auto status = d_->load();
    if (status != "Ready") {
        return {{"status", status}, {"recoveryRequired", true}};
    }
    if (!d_->present) {
        return {{"status", "Ready"}, {"state", "Empty"}, {"recoveryRequired", false}};
    }
    const bool needsRecovery = d_->state == BackupRecoveryState::Prepared ||
                               d_->state == BackupRecoveryState::Applying ||
                               d_->state == BackupRecoveryState::RecoveryRequired;
    QJsonArray inventory;
    for (const auto& value : d_->domains) {
        const auto item = value.toObject();
        inventory.append(
            QJsonObject{{"domain", item.value("domain").toInt() == 1 ? "memory" : "conversations"},
                        {"schemaVersion", item.value("schema")},
                        {"preimageBytes", item.value("bytes")}});
    }
    return {{"status", "Ready"},
            {"domains", inventory},
            {"state", backupRecoveryStateName(d_->state)},
            {"sequence", d_->sequence},
            {"domainCount", d_->domains.size()},
            {"recoveryRequired", needsRecovery},
            {"automaticReplay", false},
            {"verification", "MetadataOnly"}};
}
QString BackupRecoveryJournal::readPreimage(BackupRecoveryDomain domain, QIODevice& destination) {
    if (d_->load() != "Ready" || !d_->present || !destination.isWritable()) {
        return "InvalidInput";
    }
    for (const auto& value : d_->domains) {
        const auto item = value.toObject();
        if (item.value("domain").toInt() != static_cast<int>(domain)) {
            continue;
        }
        QByteArray sum;
        qint64 size = 0;
        // Validate before exposing any staged bytes. Destination publication belongs to caller.
        if (d_->preimage(static_cast<int>(domain), nullptr, &sum, &size) != "Ready" ||
            sum != item.value("before").toString().toLatin1() ||
            size != item.value("bytes").toInteger(-1)) {
            return "IntegrityFailure";
        }
        auto copied = d_->preimage(static_cast<int>(domain), &destination, &sum, &size);
        if (copied != "Ready") {
            return copied;
        }
        return sum == item.value("before").toString().toLatin1() &&
                       size == item.value("bytes").toInteger(-1)
                   ? "Ready"
                   : "IntegrityFailure";
    }
    return "InvalidInput";
}
} // namespace sentinel::core
