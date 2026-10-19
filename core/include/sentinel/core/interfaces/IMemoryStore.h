// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/memory/MemoryManagement.h"
#include "sentinel/core/memory/ScopedMemory.h"

#include <QList>
#include <QString>
#include <QtGlobal>
#include <utility>

namespace sentinel::core {

using MemoryEntry = std::pair<QString, QString>;
using MemoryEntries = QList<MemoryEntry>;
struct MemoryRecord {
    qint64 id = 0;
    QString key;
    QString value;
    MemoryScope scope = MemoryScope::Legacy;
    MemoryProvenance provenance = MemoryProvenance::Unknown;
};

class IMemoryStore {
public:
    Q_DISABLE_COPY(IMemoryStore)
    IMemoryStore() = default;
    virtual ~IMemoryStore() = default;

    virtual void put(QString key, QString value) = 0;
    virtual QString get(const QString& key) const = 0;
    virtual MemoryEntries entries() const = 0;
    virtual MemoryEntries searchRelevant(const QString& query, int limit) const {
        Q_UNUSED(query)
        Q_UNUSED(limit)
        return {};
    }
    virtual QList<MemoryRecord> searchRelevantRecords(const QString& query, int limit) const {
        Q_UNUSED(query)
        Q_UNUSED(limit)
        return {};
    }
    // Old key/value methods address legacy/unclassified data only. They grant no scoped visibility.
    virtual ScopedMemoryResult writeScopedMemory(const ScopedMemoryWrite&,
                                                 const MemoryAccessContext&) {
        return {};
    }
    virtual ScopedMemoryResult retrieveMemory(const QString&, const MemoryAccessContext&,
                                              int = 10) const {
        return {};
    }
    virtual MemoryManagementResult listManagedMemory(const MemoryPageQuery&,
                                                     const MemoryManagementContext&) const {
        return {};
    }
    virtual MemoryManagementResult readManagedMemory(const QString&,
                                                     const MemoryManagementContext&) const {
        return {};
    }
    virtual MemoryManagementResult mutateManagedMemory(const QList<MemoryMutation>&,
                                                       const MemoryManagementContext&) {
        return {};
    }
    virtual MemoryManagementResult clearManagedMemory(MemoryScope, const MemoryManagementContext&) {
        return {};
    }
    virtual bool hasScopedMemory() const {
        return false;
    }
    // Legacy JSON cannot preserve management identities or correction receipts.
    virtual bool hasManagedMemory() const {
        return hasScopedMemory();
    }
    virtual void clear() = 0;
    virtual bool isAvailable() const {
        return true;
    }
    virtual QString lastError() const {
        return {};
    }
};

} // namespace sentinel::core

namespace sentinel::core::interfaces {
using IMemoryStore = ::sentinel::core::IMemoryStore;
using MemoryEntry = ::sentinel::core::MemoryEntry;
using MemoryEntries = ::sentinel::core::MemoryEntries;
using MemoryRecord = ::sentinel::core::MemoryRecord;
} // namespace sentinel::core::interfaces
