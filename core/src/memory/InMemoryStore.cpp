// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/memory/InMemoryStore.h"
#include <QReadLocker>
#include <QUuid>
#include <QWriteLocker>
#include <algorithm>

namespace sentinel::core {

void InMemoryStore::put(QString key, QString value) {
    if (!ids_.contains(key))
        ids_.insert(key, nextId_++);
    entries_.insert(key, value);
}

QString InMemoryStore::get(const QString& key) const {
    return entries_.value(key);
}

MemoryEntries InMemoryStore::entries() const {
    MemoryEntries result;
    result.reserve(entries_.size());

    for (auto it = entries_.cbegin(); it != entries_.cend(); ++it) {
        result.append({it.key(), it.value()});
    }

    return result;
}

MemoryEntries InMemoryStore::searchRelevant(const QString& query, int limit) const {
    MemoryEntries result;
    for (const auto& record : searchRelevantRecords(query, limit))
        result.append({record.key, record.value});
    return result;
}

QList<MemoryRecord> InMemoryStore::searchRelevantRecords(const QString& query, int limit) const {
    QList<MemoryRecord> result;
    if (query.trimmed().isEmpty() || limit <= 0)
        return result;
    for (auto it = entries_.cbegin(); it != entries_.cend() && result.size() < limit; ++it)
        if (it.key().contains(query, Qt::CaseInsensitive) ||
            it.value().contains(query, Qt::CaseInsensitive))
            result.append({ids_.value(it.key()), it.key(), it.value()});
    return result;
}

ScopedMemoryResult InMemoryStore::writeScopedMemory(const ScopedMemoryWrite& write,
                                                    const MemoryAccessContext& context) {
    if (!permittedMemoryWrite(write, context)) {
        return {ScopedMemoryStatus::Unauthorized, {}};
    }
    if (!validMemoryWrite(write)) {
        return {ScopedMemoryStatus::InvalidInput, {}};
    }
    QWriteLocker lock(&scopedLock_);
    const auto now = QDateTime::currentDateTimeUtc();
    const auto owner = memoryOwner(write.scope, context);
    const auto workspace = memoryWorkspace(write.scope, context);
    for (auto& record : scoped_) {
        if (record.scope == write.scope && record.ownerId == owner &&
            record.workspaceId == workspace && record.key == write.key) {
            if (!write.overwriteExisting) {
                return {ScopedMemoryStatus::InvalidInput, {}};
            }
            record.value = write.value;
            record.provenance = write.provenance;
            record.sourceReference = write.sourceReference;
            record.updatedAtUtc = now;
            return {ScopedMemoryStatus::Ready, {record}};
        }
    }
    ScopedMemoryRecord record{QUuid::createUuid().toString(QUuid::WithoutBraces),
                              write.scope,
                              owner,
                              workspace,
                              write.key,
                              write.value,
                              write.provenance,
                              write.sourceReference,
                              now,
                              now};
    scoped_.append(record);
    return {ScopedMemoryStatus::Ready, {record}};
}
ScopedMemoryResult InMemoryStore::retrieveMemory(const QString& text,
                                                 const MemoryAccessContext& context,
                                                 int limit) const {
    if (!authorizedMemoryContext(context)) {
        return {ScopedMemoryStatus::Unauthorized, {}};
    }
    if (limit <= 0 || text.size() > 4096) {
        return {ScopedMemoryStatus::InvalidInput, {}};
    }
    QReadLocker lock(&scopedLock_);
    ScopedMemoryResult result{ScopedMemoryStatus::Ready, {}};
    const auto terms = scopedMemoryQueryTerms(text);
    for (const auto& record : scoped_) {
        const bool matches =
            terms.isEmpty() || std::any_of(terms.cbegin(), terms.cend(), [&](const auto& term) {
                return record.key.contains(term, Qt::CaseInsensitive) ||
                       record.value.contains(term, Qt::CaseInsensitive);
            });
        if (visibleMemory(record, context) && matches) {
            result.records.append(record);
        }
    }
    std::sort(result.records.begin(), result.records.end(), [](const auto& a, const auto& b) {
        return a.updatedAtUtc == b.updatedAtUtc ? a.id < b.id : a.updatedAtUtc > b.updatedAtUtc;
    });
    result.records = result.records.mid(0, qBound(1, limit, 50));
    return result;
}

bool InMemoryStore::hasScopedMemory() const {
    QReadLocker lock(&scopedLock_);
    return !scoped_.isEmpty();
}

void InMemoryStore::clear() {
    QWriteLocker lock(&scopedLock_);
    scoped_.clear();
    entries_.clear();
    ids_.clear();
}

} // namespace sentinel::core
