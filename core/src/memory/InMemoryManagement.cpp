// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/memory/InMemoryStore.h"
#include <QReadLocker>
#include <QWriteLocker>
#include <algorithm>
#include <utility>
namespace sentinel::core {
MemoryManagementResult InMemoryStore::listManagedMemory(const MemoryPageQuery& page,
                                                        const MemoryManagementContext& c) const {
    if (!authorizedMemoryScope(page.scope, c)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    if (page.limit < 1 || page.limit > 50 || page.search.size() > 256 || page.afterId.size() > 64) {
        return {MemoryManagementStatus::InvalidInput, {}, {}};
    }
    QReadLocker lock(&scopedLock_);
    QList<ScopedMemoryRecord> candidates;
    const auto add = [&](const ScopedMemoryRecord& r) {
        if (r.scope == page.scope && manageableMemory(r, c) && r.id > page.afterId &&
            (page.search.isEmpty() || r.key.contains(page.search, Qt::CaseInsensitive) ||
             r.value.contains(page.search, Qt::CaseInsensitive))) {
            candidates.append(r);
        }
    };
    if (page.scope == MemoryScope::Legacy) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            auto r = legacyMetadata_.value(it.key());
            r.key = it.key();
            r.value = it.value();
            add(r);
        }
    } else {
        for (const auto& r : scoped_) {
            add(r);
        }
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const auto& a, const auto& b) { return a.id < b.id; });
    MemoryManagementResult result{MemoryManagementStatus::Ready, candidates.mid(0, page.limit), {}};
    if (candidates.size() > page.limit) {
        result.nextCursor = result.records.last().id;
    }
    return result;
}
MemoryManagementResult InMemoryStore::readManagedMemory(const QString& id,
                                                        const MemoryManagementContext& c) const {
    if (!authorizedMemoryContext(c.access)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    QReadLocker lock(&scopedLock_);
    for (const auto& r : scoped_) {
        if (r.id == id && manageableMemory(r, c)) {
            return {MemoryManagementStatus::Ready, {r}, {}};
        }
    }
    if (c.allowLegacy) {
        for (auto it = legacyMetadata_.begin(); it != legacyMetadata_.end(); ++it) {
            if (it.value().id == id) {
                auto r = it.value();
                r.key = it.key();
                r.value = entries_.value(it.key());
                return {MemoryManagementStatus::Ready, {r}, {}};
            }
        }
    }
    return {MemoryManagementStatus::NotFound, {}, {}};
}
MemoryManagementResult InMemoryStore::mutateManagedMemory(const QList<MemoryMutation>& changes,
                                                          const MemoryManagementContext& c) {
    if (!authorizedMemoryContext(c.access)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    if (changes.isEmpty() || changes.size() > 500) {
        return {MemoryManagementStatus::InvalidInput, {}, {}};
    }
    QWriteLocker lock(&scopedLock_);
    auto scoped = scoped_;
    auto legacy = legacyMetadata_;
    auto values = entries_;
    auto ids = ids_;
    auto nextId = nextId_;
    MemoryManagementResult result{MemoryManagementStatus::Ready, {}, {}};
    for (const auto& m : changes) {
        ScopedMemoryRecord old;
        bool found = false;
        for (const auto& r : scoped) {
            if (r.id == m.record.id) {
                old = r;
                found = true;
                break;
            }
        }
        if (!found && c.allowLegacy) {
            for (auto it = legacy.begin(); it != legacy.end(); ++it) {
                if (it.value().id == m.record.id) {
                    old = it.value();
                    old.key = it.key();
                    old.value = values.value(it.key());
                    found = true;
                    break;
                }
            }
        }
        if (found && !manageableMemory(old, c)) {
            return {MemoryManagementStatus::NotFound, {}, {}};
        }
        ScopedMemoryRecord next;
        bool skip = false;
        const auto status = prepareManagedMutation(m, found ? &old : nullptr, c, &next, &skip);
        if (status != MemoryManagementStatus::Ready) {
            return {status, {}, {}};
        }
        if (skip) {
            continue;
        }
        const auto legacyId = ids.value(old.key);
        if (found) {
            if (old.scope == MemoryScope::Legacy) {
                legacy.remove(old.key);
                values.remove(old.key);
                ids.remove(old.key);
            } else {
                scoped.erase(std::remove_if(scoped.begin(), scoped.end(),
                                            [&](const auto& r) { return r.id == old.id; }),
                             scoped.end());
            }
        }
        if (m.kind == MemoryMutation::Kind::Delete) {
            if (old.scope == MemoryScope::Legacy) {
                ids.remove(old.key);
            }
            continue;
        }
        for (const auto& r : legacy) {
            if (r.id == next.id) {
                return {MemoryManagementStatus::Conflict, {}, {}};
            }
        }
        for (const auto& r : scoped) {
            if (r.id == next.id || (r.scope == next.scope && r.ownerId == next.ownerId &&
                                    r.workspaceId == next.workspaceId && r.key == next.key)) {
                return {MemoryManagementStatus::Conflict, {}, {}};
            }
        }
        if (next.scope == MemoryScope::Legacy) {
            if (values.contains(next.key)) {
                return {MemoryManagementStatus::Conflict, {}, {}};
            }
            legacy[next.key] = next;
            values[next.key] = next.value;
            if (!ids.contains(next.key)) {
                ids[next.key] = legacyId > 0 ? legacyId : nextId++;
            }
        } else {
            scoped.append(next);
        }
        result.records.append(next);
    }
    scoped_ = std::move(scoped);
    legacyMetadata_ = std::move(legacy);
    entries_ = std::move(values);
    ids_ = std::move(ids);
    nextId_ = nextId;
    return result;
}
MemoryManagementResult InMemoryStore::clearManagedMemory(MemoryScope scope,
                                                         const MemoryManagementContext& c) {
    if (!authorizedMemoryScope(scope, c)) {
        return {MemoryManagementStatus::Unauthorized, {}, {}};
    }
    QWriteLocker lock(&scopedLock_);
    if (scope == MemoryScope::Legacy) {
        entries_.clear();
        legacyMetadata_.clear();
        ids_.clear();
    } else {
        scoped_.erase(std::remove_if(scoped_.begin(), scoped_.end(),
                                     [&](const auto& r) {
                                         return r.scope == scope && manageableMemory(r, c);
                                     }),
                      scoped_.end());
    }
    return {MemoryManagementStatus::Ready, {}, {}};
}
} // namespace sentinel::core
