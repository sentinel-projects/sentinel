// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/MemoryArchive.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
namespace sentinel::core {
namespace {
QByteArray json(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}
QByteArray payload(const QJsonArray& rows) {
    return QJsonDocument(rows).toJson(QJsonDocument::Compact);
}
QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool cancelled(const MemoryArchiveControl& c) {
    return c.cancelled && c.cancelled();
}
bool permitted(const ScopedMemoryRecord& record, MemoryScope scope,
               const MemoryManagementContext& context) {
    return record.scope == scope && manageableMemory(record, context);
}
} // namespace
MemoryArchiveResult writeMemoryArchive(QIODevice& out, MemoryScope scope,
                                       const MemoryManagementContext& context,
                                       const MemoryArchiveLimits& limits,
                                       const MemoryArchiveControl& control,
                                       const MemoryArchiveProducer& produce) {
    MemoryArchiveResult result;
    result.version = 2;
    if (!limits.valid() || !authorizedMemoryScope(scope, context)) {
        result.status = "Unauthorized";
        return result;
    }
    QCryptographicHash complete(QCryptographicHash::Sha256), identities(QCryptographicHash::Sha256);
    auto write = [&](const QJsonObject& object, bool hashed = true) {
        const auto line = json(object) + '\n';
        if (result.bytes + line.size() > limits.maximumBytes) {
            return QString("LimitExceeded");
        }
        if (cancelled(control)) {
            return QString("Cancelled");
        }
        if (out.write(line) != line.size()) {
            return QString("WriteFailure");
        }
        result.bytes += line.size();
        if (hashed) {
            complete.addData(line);
        }
        return QString("Ready");
    };
    result.status = write({{"format", "sentinel-scoped-memory"},
                           {"version", 2},
                           {"schema", 7},
                           {"scope", static_cast<int>(scope)}});
    if (!result.ok()) {
        return result;
    }
    QJsonArray chunk;
    int chunkBytes = 2;
    QString previous;
    auto flush = [&]() {
        if (chunk.isEmpty()) {
            return QString("Ready");
        }
        const auto status = write({{"kind", "chunk"},
                                   {"index", result.chunks},
                                   {"records", chunk},
                                   {"sha256", digest(payload(chunk))}});
        if (status == "Ready") {
            ++result.chunks;
            chunk = {};
            chunkBytes = 2;
        }
        return status;
    };
    result.status = produce([&](const ScopedMemoryRecord& record) {
        if (cancelled(control)) {
            return QString("Cancelled");
        }
        if (!permitted(record, scope, context)) {
            return QString("Unauthorized");
        }
        if (record.id <= previous || result.records >= limits.maximumRecords) {
            return QString("LimitExceeded");
        }
        auto metadata = managedMemoryJson(record);
        // Restore must append its own attributed import receipt without dropping history.
        if (record.corrections.size() >= memoryHistoryLimit) {
            return QString("LimitExceeded");
        }
        metadata.remove("revision");
        ScopedMemoryRecord checked;
        if (!parseManagedMemory(metadata, &checked) || record.revision.isEmpty() ||
            record.revision.size() > 64) {
            return QString("InvalidInput");
        }
        const QJsonObject row{{"record", metadata}, {"exportRevision", record.revision}};
        const int size = static_cast<int>(json(row).size()) + 1;
        if (size > memoryArchiveChunkBytes - 1024) {
            return QString("LimitExceeded");
        }
        if (chunk.size() >= 100 || chunkBytes + size > memoryArchiveChunkBytes - 1024) {
            const auto status = flush();
            if (status != "Ready") {
                return status;
            }
        }
        chunk.append(row);
        chunkBytes += size;
        previous = record.id;
        ++result.records;
        identities.addData(record.id.toUtf8() + '\n');
        if (control.progress) {
            control.progress(result.records, result.bytes);
        }
        return QString("Ready");
    });
    if (!result.ok()) {
        return result;
    }
    result.status = flush();
    if (!result.ok()) {
        return result;
    }
    result.digest = QString::fromLatin1(complete.result().toHex());
    result.status = write({{"kind", "manifest"},
                           {"version", 2},
                           {"schema", 7},
                           {"scope", static_cast<int>(scope)},
                           {"chunks", result.chunks},
                           {"records", result.records},
                           {"payloadSha256", result.digest},
                           {"identitiesSha256", QString::fromLatin1(identities.result().toHex())}},
                          false);
    return result;
}
MemoryArchiveResult readMemoryArchive(QIODevice& in, MemoryScope scope,
                                      const MemoryManagementContext& context,
                                      const MemoryArchiveLimits& limits,
                                      const MemoryArchiveControl& control,
                                      const MemoryArchiveConsumer& consume) {
    MemoryArchiveResult result;
    if (!limits.valid() || !authorizedMemoryScope(scope, context)) {
        result.status = "Unauthorized";
        return result;
    }
    if (in.size() > limits.maximumBytes || !in.seek(0)) {
        result.status = "LimitExceeded";
        return result;
    }
    auto first = in.readLine(memoryArchiveChunkBytes + 1);
    const auto header = QJsonDocument::fromJson(first).object();
    if (header.value("version") != 2) {
        if (header.contains("version") && header.value("version") != 1) {
            result.status = "UnsupportedVersion";
            return result;
        }
        if (!in.seek(0)) {
            return result;
        }
        const auto bytes = in.read(98305);
        const auto decoded = decodeMemoryArchive(bytes);
        result.version = 1;
        result.bytes = bytes.size();
        if (decoded.status != MemoryManagementStatus::Ready || !in.atEnd()) {
            return result;
        }
        if (decoded.records.size() > limits.maximumRecords) {
            result.status = "LimitExceeded";
            return result;
        }
        for (const auto& r : decoded.records) {
            if (cancelled(control)) {
                result.status = "Cancelled";
                return result;
            }
            if (!permitted(r, scope, context)) {
                result.status = "Unauthorized";
                return result;
            }
            if (consume) {
                result.status = consume(r);
                if (!result.ok()) {
                    return result;
                }
            }
            ++result.records;
            if (control.progress) {
                control.progress(result.records, result.bytes);
            }
        }
        result.status = "Ready";
        result.digest = digest(bytes);
        return result;
    }
    result.version = 2;
    if (header.size() != 4 || header.value("format") != "sentinel-scoped-memory" ||
        header.value("schema") != 7 || header.value("scope") != static_cast<int>(scope) ||
        !first.endsWith('\n')) {
        return result;
    }
    QCryptographicHash complete(QCryptographicHash::Sha256), identities(QCryptographicHash::Sha256);
    complete.addData(first);
    result.bytes = first.size();
    QString previous;
    while (!in.atEnd()) {
        if (cancelled(control)) {
            result.status = "Cancelled";
            return result;
        }
        const auto line = in.readLine(memoryArchiveChunkBytes + 1);
        result.bytes += line.size();
        if (result.bytes > limits.maximumBytes || line.size() > memoryArchiveChunkBytes) {
            result.status = "LimitExceeded";
            return result;
        }
        if (!line.endsWith('\n')) {
            result.status = "Truncated";
            return result;
        }
        const auto object = QJsonDocument::fromJson(line).object();
        if (object.value("kind") == "manifest") {
            if (object.size() != 8 || object.value("version") != 2 || object.value("schema") != 7 ||
                object.value("scope") != static_cast<int>(scope) ||
                object.value("chunks") != result.chunks ||
                object.value("records").toDouble(-1) != static_cast<double>(result.records) ||
                object.value("payloadSha256") != QString::fromLatin1(complete.result().toHex()) ||
                object.value("identitiesSha256") !=
                    QString::fromLatin1(identities.result().toHex()) ||
                !in.atEnd()) {
                return result;
            }
            result.digest = object.value("payloadSha256").toString();
            result.status = "Ready";
            return result;
        }
        if (object.size() != 4 || object.value("kind") != "chunk" ||
            object.value("index") != result.chunks || !object.value("records").isArray()) {
            return result;
        }
        const auto rows = object.value("records").toArray();
        if (rows.isEmpty() || rows.size() > 100 ||
            object.value("sha256") != digest(payload(rows))) {
            return result;
        }
        for (const auto& value : rows) {
            if (cancelled(control)) {
                result.status = "Cancelled";
                return result;
            }
            const auto row = value.toObject();
            ScopedMemoryRecord record;
            if (row.size() != 2 || !row.value("record").isObject() ||
                !row.value("exportRevision").isString() ||
                row.value("exportRevision").toString().isEmpty() ||
                row.value("exportRevision").toString().size() > 64 ||
                !parseManagedMemory(row.value("record").toObject(), &record) ||
                record.id <= previous) {
                return result;
            }
            if (record.corrections.size() >= memoryHistoryLimit ||
                ++result.records > limits.maximumRecords) {
                result.status = "LimitExceeded";
                return result;
            }
            if (!permitted(record, scope, context)) {
                result.status = "Unauthorized";
                return result;
            }
            previous = record.id;
            identities.addData(record.id.toUtf8() + '\n');
            if (consume) {
                result.status = consume(record);
                if (!result.ok()) {
                    return result;
                }
                result.status = "InvalidInput";
            }
        }
        complete.addData(line);
        ++result.chunks;
        if (control.progress) {
            control.progress(result.records, result.bytes);
        }
    }
    result.status = "Truncated";
    return result;
}
} // namespace sentinel::core
