// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/memory/SQLiteMemoryStore.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using sentinel::core::SQLiteMemoryStore;

class SQLiteMemoryStoreTest final : public QObject {
    Q_OBJECT

private slots:
    void returnsEmptyStringForMissingKeys();
    void storesAndRetrievesValues();
    void overwritesExistingKeys();
    void handlesEmptyKeysAsStorageData();
    void handlesRepeatedOverwrites();
    void keepsMultipleKeysIndependent();
    void exposesAllEntriesDeterministically();
    void clearsPersistedValues();
    void persistsValuesAcrossInstances();
    void initializesSchemaVersion();
    void reportsAvailabilityForValidDatabase();
    void safelyNoOpsForUnopenableDatabasePath();
    void createsParentDirectories();
};

static QString databasePath(QTemporaryDir& dir,
                            const QString& fileName = QStringLiteral("memory.sqlite3")) {
    return dir.filePath(fileName);
}

void SQLiteMemoryStoreTest::returnsEmptyStringForMissingKeys() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    QVERIFY(store.get(QStringLiteral("missing")).isEmpty());
    QVERIFY(store.entries().isEmpty());
}

void SQLiteMemoryStoreTest::storesAndRetrievesValues() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    store.put(QStringLiteral("callsign"), QStringLiteral("Sentinel"));

    QCOMPARE(store.get(QStringLiteral("callsign")), QStringLiteral("Sentinel"));
    QCOMPARE(store.entries().size(), 1);
}

void SQLiteMemoryStoreTest::overwritesExistingKeys() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    store.put(QStringLiteral("mode"), QStringLiteral("Companion"));
    store.put(QStringLiteral("mode"), QStringLiteral("Tactical"));

    QCOMPARE(store.get(QStringLiteral("mode")), QStringLiteral("Tactical"));
    QCOMPARE(store.entries().size(), 1);
}

void SQLiteMemoryStoreTest::handlesEmptyKeysAsStorageData() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    store.put(QString(), QStringLiteral("empty-key-value"));

    QCOMPARE(store.get(QString()), QStringLiteral("empty-key-value"));
    QCOMPARE(store.entries().size(), 1);
    QCOMPARE(store.entries().first().first, QString());
}

void SQLiteMemoryStoreTest::handlesRepeatedOverwrites() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    store.put(QStringLiteral("status"), QStringLiteral("one"));
    store.put(QStringLiteral("status"), QStringLiteral("two"));
    store.put(QStringLiteral("status"), QStringLiteral("three"));

    QCOMPARE(store.get(QStringLiteral("status")), QStringLiteral("three"));
    QCOMPARE(store.entries().size(), 1);
}

void SQLiteMemoryStoreTest::keepsMultipleKeysIndependent() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    store.put(QStringLiteral("mode"), QStringLiteral("Companion"));
    store.put(QStringLiteral("provider"), QStringLiteral("LocalEchoProvider"));
    store.put(QStringLiteral("mode"), QStringLiteral("Mission"));

    QCOMPARE(store.get(QStringLiteral("mode")), QStringLiteral("Mission"));
    QCOMPARE(store.get(QStringLiteral("provider")), QStringLiteral("LocalEchoProvider"));
    QCOMPARE(store.entries().size(), 2);
}

void SQLiteMemoryStoreTest::exposesAllEntriesDeterministically() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    store.put(QStringLiteral("zulu"), QStringLiteral("last"));
    store.put(QStringLiteral("alpha"), QStringLiteral("first"));

    const auto entries = store.entries();

    QCOMPARE(entries.size(), 2);
    QCOMPARE(entries.at(0).first, QStringLiteral("alpha"));
    QCOMPARE(entries.at(0).second, QStringLiteral("first"));
    QCOMPARE(entries.at(1).first, QStringLiteral("zulu"));
    QCOMPARE(entries.at(1).second, QStringLiteral("last"));
}

void SQLiteMemoryStoreTest::clearsPersistedValues() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    store.put(QStringLiteral("alpha"), QStringLiteral("first"));
    store.put(QStringLiteral("zulu"), QStringLiteral("last"));
    store.clear();

    QVERIFY(store.entries().isEmpty());
    QVERIFY(store.get(QStringLiteral("alpha")).isEmpty());
    QVERIFY(store.lastError().isEmpty());
}

void SQLiteMemoryStoreTest::persistsValuesAcrossInstances() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = databasePath(dir);

    {
        SQLiteMemoryStore store(path);
        store.put(QStringLiteral("callsign"), QStringLiteral("Sentinel"));
        store.put(QStringLiteral("mode"), QStringLiteral("Tactical"));
    }

    SQLiteMemoryStore reloaded(path);

    QCOMPARE(reloaded.get(QStringLiteral("callsign")), QStringLiteral("Sentinel"));
    QCOMPARE(reloaded.get(QStringLiteral("mode")), QStringLiteral("Tactical"));
    QCOMPARE(reloaded.entries().size(), 2);
}

void SQLiteMemoryStoreTest::initializesSchemaVersion() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    QCOMPARE(store.schemaVersion(), 6);
}

void SQLiteMemoryStoreTest::reportsAvailabilityForValidDatabase() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(databasePath(dir));

    QVERIFY(store.isAvailable());
    QVERIFY(store.lastError().isEmpty());
}

void SQLiteMemoryStoreTest::safelyNoOpsForUnopenableDatabasePath() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    SQLiteMemoryStore store(dir.path());

    QVERIFY(!store.isAvailable());
    QVERIFY(!store.lastError().isEmpty());

    store.put(QStringLiteral("mode"), QStringLiteral("Tactical"));

    QVERIFY(store.get(QStringLiteral("mode")).isEmpty());
    QVERIFY(store.entries().isEmpty());
    QCOMPARE(store.schemaVersion(), 0);
}

void SQLiteMemoryStoreTest::createsParentDirectories() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = databasePath(dir, QStringLiteral("nested/storage/memory.sqlite3"));

    SQLiteMemoryStore store(path);
    store.put(QStringLiteral("callsign"), QStringLiteral("Sentinel"));

    QVERIFY(QFile::exists(path));
}

QTEST_MAIN(SQLiteMemoryStoreTest)

#include "test_sqlite_memory_store.moc"
