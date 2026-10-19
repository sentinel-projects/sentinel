// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/MemoryManagement.h"
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUuid>
namespace sentinel::core {
bool authorizedMemoryScope(MemoryScope scope, const MemoryManagementContext& c) {
    if (!authorizedMemoryContext(c.access)) {
        return false;
    }
    switch (scope) {
    case MemoryScope::Session:
        return true;
    case MemoryScope::Project:
        return !c.access.workspaceId.isEmpty();
    case MemoryScope::Personal:
        return c.access.includePersonal;
    case MemoryScope::Legacy:
        return c.allowLegacy;
    }
    return false;
}
bool manageableMemory(const ScopedMemoryRecord& r, const MemoryManagementContext& c) {
    return authorizedMemoryScope(r.scope, c) &&
           (r.scope == MemoryScope::Legacy ? r.ownerId.isEmpty() && r.workspaceId.isEmpty()
                                           : visibleMemory(r, c.access));
}
QString memoryManagementStatusName(MemoryManagementStatus s) {
    switch (s) {
    case MemoryManagementStatus::Ready:
        return "Ready";
    case MemoryManagementStatus::Unavailable:
        return "Unavailable";
    case MemoryManagementStatus::Unauthorized:
        return "Unauthorized";
    case MemoryManagementStatus::InvalidInput:
        return "InvalidInput";
    case MemoryManagementStatus::Conflict:
        return "Conflict";
    case MemoryManagementStatus::NotFound:
        return "NotFound";
    case MemoryManagementStatus::LimitExceeded:
        return "LimitExceeded";
    }
    return "Unavailable";
}
QJsonObject managedMemoryJson(const ScopedMemoryRecord& r, bool preview) {
    return {{"id", r.id},
            {"scope", static_cast<int>(r.scope)},
            {"owner", r.ownerId},
            {"workspace", r.workspaceId},
            {"key", r.key},
            {"value", preview ? r.value.left(300) : r.value},
            {"valueLength", r.value.size()},
            {"provenance", static_cast<int>(r.provenance)},
            {"source", r.sourceReference},
            {"created", r.createdAtUtc.isValid()
                            ? QJsonValue(r.createdAtUtc.toUTC().toString(Qt::ISODateWithMs))
                            : QJsonValue(QJsonValue::Null)},
            {"updated", r.updatedAtUtc.isValid()
                            ? QJsonValue(r.updatedAtUtc.toUTC().toString(Qt::ISODateWithMs))
                            : QJsonValue(QJsonValue::Null)},
            {"revision", r.revision},
            {"corrections", preview ? QJsonArray{} : r.corrections},
            {"correctionCount", r.corrections.size()},
            {"recallEligible", r.scope != MemoryScope::Legacy && r.scope != MemoryScope::Personal},
            {"untrusted", true}};
}
bool parseManagedMemory(const QJsonObject& o, ScopedMemoryRecord* r) {
    const QSet<QString> allowed{
        "id",      "scope",       "owner",           "workspace",      "key",
        "value",   "valueLength", "provenance",      "source",         "created",
        "updated", "corrections", "correctionCount", "recallEligible", "untrusted"};
    for (auto it = o.begin(); it != o.end(); ++it) {
        if (!allowed.contains(it.key())) {
            return false;
        }
    }
    for (const auto& key : {"id", "owner", "workspace", "key", "value", "source"}) {
        if (!o.value(key).isString()) {
            return false;
        }
    }
    if (!o.value("scope").isDouble() || o.value("scope").toDouble() != o.value("scope").toInt() ||
        o.value("scope").toInt() < 0 || o.value("scope").toInt() > 3 ||
        !o.value("provenance").isDouble() ||
        o.value("provenance").toDouble() != o.value("provenance").toInt() ||
        o.value("provenance").toInt() < 0 || o.value("provenance").toInt() > 6 ||
        !o.value("corrections").isArray()) {
        return false;
    }
    r->id = o.value("id").toString();
    r->scope = static_cast<MemoryScope>(o.value("scope").toInt());
    r->ownerId = o.value("owner").toString();
    r->workspaceId = o.value("workspace").toString();
    r->key = o.value("key").toString();
    r->value = o.value("value").toString();
    r->provenance = static_cast<MemoryProvenance>(o.value("provenance").toInt());
    r->sourceReference = o.value("source").toString();
    r->createdAtUtc = QDateTime::fromString(o.value("created").toString(), Qt::ISODateWithMs);
    r->updatedAtUtc = QDateTime::fromString(o.value("updated").toString(), Qt::ISODateWithMs);
    r->corrections = o.value("corrections").toArray();
    if (QUuid(r->id).isNull() ||
        (r->scope == MemoryScope::Legacy
             ? r->key.size() > 256 || r->value.size() > 65536 || r->sourceReference.size() > 512
             : !validMemoryWrite(
                   {r->scope, r->key, r->value, r->provenance, r->sourceReference})) ||
        r->ownerId.size() > 256 || r->workspaceId.size() > 256 || r->corrections.size() > 64) {
        return false;
    }
    if (r->scope != MemoryScope::Legacy &&
        (!r->createdAtUtc.isValid() || !r->updatedAtUtc.isValid() ||
         r->updatedAtUtc < r->createdAtUtc)) {
        return false;
    }
    if (r->scope == MemoryScope::Legacy && (!r->ownerId.isEmpty() || !r->workspaceId.isEmpty() ||
                                            r->provenance != MemoryProvenance::Unknown)) {
        return false;
    }
    if (QUuid(r->id).toString(QUuid::WithoutBraces) != r->id) {
        return false;
    }
    if (r->scope == MemoryScope::Session && r->ownerId.trimmed().isEmpty()) {
        return false;
    }
    if (r->scope == MemoryScope::Project &&
        (r->ownerId.isEmpty() || r->ownerId != r->workspaceId)) {
        return false;
    }
    if (r->scope == MemoryScope::Personal && (!r->ownerId.isEmpty() || !r->workspaceId.isEmpty())) {
        return false;
    }
    for (const auto& key : {"created", "updated"}) {
        if (!o.value(key).isNull() &&
            (!o.value(key).isString() ||
             !QDateTime::fromString(o.value(key).toString(), Qt::ISODateWithMs).isValid())) {
            return false;
        }
    }
    if (!o.value("untrusted").isBool() || !o.value("untrusted").toBool() ||
        !o.value("recallEligible").isBool() ||
        o.value("recallEligible").toBool() !=
            (r->scope == MemoryScope::Session || r->scope == MemoryScope::Project) ||
        o.value("valueLength").toDouble(-1) != static_cast<double>(r->value.size()) ||
        o.value("correctionCount").toDouble(-1) != static_cast<double>(r->corrections.size())) {
        return false;
    }
    // Correction receipts contain attribution, not executable data or fabricated source evidence.
    for (const auto& value : r->corrections) {
        if (!value.isObject()) {
            return false;
        }
        const auto receipt = value.toObject();
        if (receipt.size() != 4 || !receipt.value("at").isString() ||
            !QDateTime::fromString(receipt.value("at").toString(), Qt::ISODateWithMs).isValid() ||
            !receipt.value("actor").isString() || receipt.value("actor").toString().isEmpty() ||
            receipt.value("actor").toString().size() > 256 || !receipt.value("action").isString() ||
            !QSet<QString>{"correct", "move", "reclassify", "import-replace", "import"}.contains(
                receipt.value("action").toString()) ||
            !receipt.value("previousDigest").isString() ||
            !QRegularExpression("^[0-9a-f]{64}$")
                 .match(receipt.value("previousDigest").toString())
                 .hasMatch()) {
            return false;
        }
    }
    return true;
}
MemoryManagementStatus prepareManagedMutation(const MemoryMutation& m,
                                              const ScopedMemoryRecord* current,
                                              const MemoryManagementContext& context,
                                              ScopedMemoryRecord* next, bool* skip) {
    *skip = false;
    if (current && !manageableMemory(*current, context)) {
        return MemoryManagementStatus::Unauthorized;
    }
    if (m.kind != MemoryMutation::Kind::Create && m.kind != MemoryMutation::Kind::Import) {
        if (!current) {
            return MemoryManagementStatus::NotFound;
        }
        if (m.expectedRevision.isEmpty() || current->revision != m.expectedRevision) {
            return MemoryManagementStatus::Conflict;
        }
    }
    const auto now = QDateTime::currentDateTimeUtc();
    if (m.kind == MemoryMutation::Kind::Delete) {
        *next = *current;
        return MemoryManagementStatus::Ready;
    }
    if (current) {
        auto metadata = managedMemoryJson(*current);
        metadata.remove("revision");
        ScopedMemoryRecord valid;
        if (!parseManagedMemory(metadata, &valid)) {
            return MemoryManagementStatus::InvalidInput;
        }
    }
    if (m.kind == MemoryMutation::Kind::Import) {
        auto serialized = managedMemoryJson(m.record);
        serialized.remove("revision");
        ScopedMemoryRecord validated;
        if (!parseManagedMemory(serialized, &validated)) {
            return MemoryManagementStatus::InvalidInput;
        }
        if (!manageableMemory(m.record, context)) {
            return MemoryManagementStatus::Unauthorized;
        }
        if (current) {
            auto a = managedMemoryJson(*current), b = managedMemoryJson(m.record);
            for (const auto& key : {"revision", "correctionCount"}) {
                a.remove(key);
                b.remove(key);
            }
            const auto withoutImportReceipts = [](const QJsonArray& receipts) {
                QJsonArray retained;
                for (const auto& receipt : receipts) {
                    if (receipt.toObject().value("action") != "import") {
                        retained.append(receipt);
                    }
                }
                return retained;
            };
            a["corrections"] = withoutImportReceipts(current->corrections);
            b["corrections"] = withoutImportReceipts(m.record.corrections);
            if (m.skipIdentical && a == b) {
                *skip = true;
                return MemoryManagementStatus::Ready;
            }
            return MemoryManagementStatus::Conflict;
        }
        *next = m.record;
    } else if (m.kind == MemoryMutation::Kind::Create) {
        if (current || m.record.scope == MemoryScope::Legacy ||
            (m.record.provenance != MemoryProvenance::Unknown &&
             m.record.provenance != MemoryProvenance::UserProvided)) {
            return MemoryManagementStatus::InvalidInput;
        }
        *next = m.record;
        next->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        next->ownerId = memoryOwner(next->scope, context.access);
        next->workspaceId = memoryWorkspace(next->scope, context.access);
        next->createdAtUtc = next->updatedAtUtc = now;
        next->provenance = MemoryProvenance::UserProvided;
        next->sourceReference = "manual:" + context.access.sessionId;
        next->corrections = {};
    } else {
        *next = *current;
        if (next->corrections.size() >= 64) {
            return MemoryManagementStatus::LimitExceeded;
        }
        const QString action = m.kind == MemoryMutation::Kind::Move
                                   ? (current->scope == MemoryScope::Legacy ? "reclassify" : "move")
                                   : "correct";
        next->corrections.append(QJsonObject{
            {"at", now.toString(Qt::ISODateWithMs)},
            {"actor", context.access.sessionId},
            {"action", action},
            {"previousDigest",
             QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(managedMemoryJson(*current))
                                                              .toJson(QJsonDocument::Compact),
                                                          QCryptographicHash::Sha256)
                                     .toHex())}});
        next->updatedAtUtc = current->updatedAtUtc.isValid() && now <= current->updatedAtUtc
                                 ? current->updatedAtUtc.addMSecs(1)
                                 : now;
        if (m.kind == MemoryMutation::Kind::Correct) {
            next->key = m.record.key;
            next->value = m.record.value;
        } else {
            if (m.destination == MemoryScope::Legacy) {
                return MemoryManagementStatus::InvalidInput;
            }
            next->scope = m.destination;
            next->ownerId = memoryOwner(next->scope, context.access);
            next->workspaceId = memoryWorkspace(next->scope, context.access);
            if (!next->createdAtUtc.isValid()) {
                next->createdAtUtc =
                    now; // New scoped classification, not historical creation evidence.
            }
        }
    }
    if (!manageableMemory(*next, context)) {
        return MemoryManagementStatus::Unauthorized;
    }
    if (next->scope == MemoryScope::Legacy
            ? next->key.size() > 256 || next->value.size() > 65536
            : !validMemoryWrite(
                  {next->scope, next->key, next->value, next->provenance, next->sourceReference})) {
        return MemoryManagementStatus::InvalidInput;
    }
    if (m.kind == MemoryMutation::Kind::Import) {
        if (next->corrections.size() >= 64) {
            return MemoryManagementStatus::LimitExceeded;
        }
        next->corrections.append(QJsonObject{
            {"at", now.toString(Qt::ISODateWithMs)},
            {"actor", context.access.sessionId},
            {"action", "import"},
            {"previousDigest",
             QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(managedMemoryJson(m.record))
                                                              .toJson(QJsonDocument::Compact),
                                                          QCryptographicHash::Sha256)
                                     .toHex())}});
    }
    next->revision = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return MemoryManagementStatus::Ready;
}
QByteArray encodeMemoryArchive(const QList<ScopedMemoryRecord>& records) {
    QJsonArray rows;
    for (const auto& r : records) {
        auto row = managedMemoryJson(r);
        row.remove("revision");
        rows.append(row);
    }
    const auto payload = QJsonDocument(rows).toJson(QJsonDocument::Compact);
    const QJsonObject root{
        {"format", "sentinel-scoped-memory"},
        {"version", 1},
        {"records", rows},
        {"sha256", QString::fromLatin1(
                       QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex())}};
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
MemoryManagementResult decodeMemoryArchive(const QByteArray& bytes) {
    MemoryManagementResult result{MemoryManagementStatus::InvalidInput, {}, {}};
    if (bytes.size() > 98304) {
        result.status = MemoryManagementStatus::LimitExceeded;
        return result;
    }
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        return result;
    }
    const auto root = doc.object();
    if (root.size() != 4 || root.value("format") != "sentinel-scoped-memory" ||
        root.value("version") != 1 || !root.value("records").isArray() ||
        !root.value("sha256").isString()) {
        return result;
    }
    const auto rows = root.value("records").toArray();
    if (rows.size() > 500) {
        result.status = MemoryManagementStatus::LimitExceeded;
        return result;
    }
    if (root.value("sha256").toString() !=
        QString::fromLatin1(
            QCryptographicHash::hash(QJsonDocument(rows).toJson(QJsonDocument::Compact),
                                     QCryptographicHash::Sha256)
                .toHex())) {
        return result;
    }
    QSet<QString> ids;
    for (const auto& row : rows) {
        ScopedMemoryRecord record;
        if (!row.isObject() || !parseManagedMemory(row.toObject(), &record) ||
            ids.contains(record.id)) {
            return result;
        }
        ids.insert(record.id);
        result.records.append(record);
    }
    result.status = MemoryManagementStatus::Ready;
    return result;
}
} // namespace sentinel::core
