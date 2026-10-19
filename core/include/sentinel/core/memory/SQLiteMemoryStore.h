// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/interfaces/IMemoryStore.h"

#include <QSqlDatabase>
#include <QString>
#include <QThread>

namespace sentinel::core {

class SQLiteMemoryStore final : public IMemoryStore {
public:
    explicit SQLiteMemoryStore(QString databasePath);
    ~SQLiteMemoryStore() override;

    SQLiteMemoryStore(const SQLiteMemoryStore&) = delete;
    SQLiteMemoryStore& operator=(const SQLiteMemoryStore&) = delete;
    SQLiteMemoryStore(SQLiteMemoryStore&&) = delete;
    SQLiteMemoryStore& operator=(SQLiteMemoryStore&&) = delete;

    void put(QString key, QString value) override;
    QString get(const QString& key) const override;
    MemoryEntries entries() const override;
    MemoryEntries searchRelevant(const QString& query, int limit) const override;
    QList<MemoryRecord> searchRelevantRecords(const QString& query, int limit) const override;
    ScopedMemoryResult writeScopedMemory(const ScopedMemoryWrite&,
                                         const MemoryAccessContext&) override;
    ScopedMemoryResult retrieveMemory(const QString&, const MemoryAccessContext&,
                                      int limit = 10) const override;
    bool hasScopedMemory() const override;
    void clear() override;
    bool isAvailable() const override;
    QString lastError() const override;

    QString databasePath() const;
    int schemaVersion() const;

private:
    void open();
    void initializeSchema();
    void setLastError(QString error) const;

    static constexpr int currentSchemaVersion = 4;

    QString databasePath_;
    QString connectionName_;
    QThread* const connectionThread_ = QThread::currentThread();
    QSqlDatabase database_;
    bool identityReady_ = false;
    bool ftsReady_ = false;
    mutable QString lastError_;
};

} // namespace sentinel::core
