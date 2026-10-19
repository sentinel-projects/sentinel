// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/agent/ContextEngine.h"
#include "sentinel/core/app/BackupService.h"
#include "sentinel/core/memory/MemoryArchiveJobs.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QBuffer>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>
#include <cstdlib>
using namespace sentinel::core;
namespace {
MemoryManagementContext own() {
    return {{"session", "project", {"project"}, false}, false};
}
ScopedMemoryRecord seed(SQLiteMemoryStore& store, QString key, QString value = "original") {
    return store
        .writeScopedMemory({MemoryScope::Project, key, value, MemoryProvenance::GeneratedSummary,
                            "summary:fixture"},
                           own().access)
        .records.first();
}
QByteArray archive(SQLiteMemoryStore& source, int* chunks = nullptr,
                   MemoryArchiveControl control = {}) {
    QBuffer buffer;
    buffer.open(QIODevice::ReadWrite);
    const auto result = SQLiteMemoryStore::exportArchiveSnapshot(
        source.databasePath(), buffer, MemoryScope::Project, own(), {}, control);
    if (chunks) {
        *chunks = result.chunks;
    }
    return result.ok() ? buffer.data() : QByteArray{};
}
MemoryArchiveResult restore(SQLiteMemoryStore& store, QByteArray bytes, bool skip = false,
                            MemoryArchiveControl control = {}) {
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    return store.restoreArchive(buffer, MemoryScope::Project, own(), {}, control, skip);
}
class FullDisk final : public QIODevice {
public:
    FullDisk() {
        open(QIODevice::WriteOnly);
    }
    qint64 readData(char*, qint64) override {
        return -1;
    }
    qint64 writeData(const char*, qint64 length) override {
        if (written + length > 500) {
            return -1;
        }
        written += length;
        return length;
    }
    qint64 written = 0;
};
} // namespace
class MemoryArchiveTest final : public QObject {
    Q_OBJECT
private slots:
    void largeRoundTripAndConsistentSnapshot() {
        QTemporaryDir dir;
        SQLiteMemoryStore source(dir.filePath("source.db")), target(dir.filePath("target.db"));
        QList<QString> ids;
        for (int i = 0; i < 650; ++i) {
            ids.append(seed(source, QString("key-%1").arg(i),
                            i == 649 ? QString("orchid") : QString(400, 'x') + " orchid")
                           .id);
        }
        bool wrote = false;
        MemoryArchiveControl concurrent;
        concurrent.progress = [&](qint64 records, qint64) {
            if (records == 1 && !wrote) {
                wrote = true;
                seed(source, "key-649", "CONCURRENT_NEW");
                seed(source, "added-during-export", "not in snapshot");
            }
        };
        int chunks = 0;
        const auto bytes = archive(source, &chunks, concurrent);
        QVERIFY(!bytes.isEmpty());
        QVERIFY(bytes.size() > 98304);
        QVERIFY(chunks >= 7);
        QVERIFY(wrote);
        QVERIFY(!bytes.contains("CONCURRENT_NEW"));
        QVERIFY(!bytes.contains("added-during-export"));
        auto restored = restore(target, bytes);
        QVERIFY2(restored.ok(), qPrintable(restored.status));
        QCOMPARE(restored.records, 650);
        QCOMPARE(restored.version, 2);
        QCOMPARE(target.memoryIndexStatus(own().access).state, MemoryIndexState::Ready);
        auto exact = target.retrieveMemory("key-649", own().access).records.first();
        QCOMPARE(exact.id, ids.last());
        QVERIFY(isImportedMemory(exact));
        QCOMPARE(exact.provenance, MemoryProvenance::GeneratedSummary);
        QVERIFY(!restore(target, bytes).ok());
        QVERIFY(restore(target, bytes, true).ok());
        AgentContextInput input;
        input.goal = "key-649";
        input.memoryStore = &target;
        input.memoryAccess = own().access;
        bool found = false;
        for (const auto& item : ContextEngine{}.build(input).items) {
            if (item.kind == AgentContextKind::Memory && item.source == ids.last()) {
                found = true;
            }
        }
        QVERIFY(found);
        SQLiteMemoryStore reopened(dir.filePath("target.db"));
        QCOMPARE(reopened.retrieveMemory("key-649", own().access).records.first().id, ids.last());
    }
    void byteChunkBoundariesAndFutureVersion() {
        QTemporaryDir dir;
        SQLiteMemoryStore source(dir.filePath("bytes.db")), target(dir.filePath("target.db"));
        const QString value(65536, QChar(1));
        for (int i = 0; i < 5; ++i) {
            seed(source, QString("bytes%1").arg(i), value);
        }
        int chunks = 0;
        const auto bytes = archive(source, &chunks);
        QVERIFY(bytes.size() > 1024 * 1024);
        QVERIFY(chunks >= 3);
        QVERIFY(restore(target, bytes).ok());
        QCOMPARE(target.retrieveMemory("bytes4", own().access).records.first().value, value);
        auto lines = bytes.split('\n');
        auto header = QJsonDocument::fromJson(lines.first()).object();
        header["version"] = 99;
        lines[0] = QJsonDocument(header).toJson(QJsonDocument::Compact);
        auto future = lines.join('\n');
        QBuffer input(&future);
        input.open(QIODevice::ReadOnly);
        QCOMPARE(readMemoryArchive(input, MemoryScope::Project, own(), {}, {}).status,
                 QString("UnsupportedVersion"));
    }
    void malformedChunksAndAtomicRollback() {
        QTemporaryDir dir;
        SQLiteMemoryStore source(dir.filePath("source.db"));
        for (int i = 0; i < 220; ++i) {
            seed(source, QString("key%1").arg(i));
        }
        const auto bytes = archive(source);
        QVERIFY(!bytes.isEmpty());
        const auto lines = bytes.split('\n');
        QVERIFY(lines.size() >= 6);
        QList<QByteArray> bad;
        auto missing = lines;
        missing.removeAt(1);
        bad.append(missing.join('\n'));
        auto duplicate = lines;
        duplicate.insert(2, duplicate[1]);
        bad.append(duplicate.join('\n'));
        auto reorder = lines;
        reorder.swapItemsAt(1, 2);
        bad.append(reorder.join('\n'));
        auto manifest = lines;
        auto object = QJsonDocument::fromJson(manifest[manifest.size() - 2]).object();
        object["records"] = 221;
        manifest[manifest.size() - 2] = QJsonDocument(object).toJson(QJsonDocument::Compact);
        bad.append(manifest.join('\n'));
        bad.append(bytes.left(bytes.size() - 15));
        auto corrupt = bytes;
        corrupt[corrupt.indexOf("original")] = '!';
        bad.append(corrupt);
        for (int i = 0; i < bad.size(); ++i) {
            SQLiteMemoryStore target(dir.filePath(QString("target%1.db").arg(i)));
            seed(target, "keep", "untouched");
            const auto result = restore(target, bad[i]);
            QVERIFY(!result.ok());
            QCOMPARE(target.listManagedMemory({MemoryScope::Project}, own()).records.size(), 1);
            QCOMPARE(target.get("keep"), QString{});
            QCOMPARE(target.retrieveMemory("keep", own().access).records.first().value,
                     QString("untouched"));
            QCOMPARE(target.memoryIndexStatus(own().access).state, MemoryIndexState::Ready);
        }
        auto foreign = own();
        foreign.access.workspaceId = "foreign";
        foreign.access.authorizedWorkspaceIds = {"foreign"};
        auto input = bytes;
        QBuffer buffer(&input);
        buffer.open(QIODevice::ReadOnly);
        QCOMPARE(readMemoryArchive(buffer, MemoryScope::Project, foreign, {}, {}).status,
                 QString("Unauthorized"));
    }
    void cancellationLimitsAndWriteFailure() {
        QTemporaryDir dir;
        SQLiteMemoryStore source(dir.filePath("source.db")), target(dir.filePath("target.db"));
        for (int i = 0; i < 110; ++i) {
            seed(source, QString("key%1").arg(i));
        }
        const auto bytes = archive(source);
        int checks = 0;
        MemoryArchiveControl stop;
        stop.cancelled = [&] { return ++checks > 5; };
        QCOMPARE(restore(target, bytes, false, stop).status, QString("Cancelled"));
        QCOMPARE(target.listManagedMemory({MemoryScope::Project}, own()).records.size(), 0);
        int processed = 0;
        MemoryArchiveControl exportStop;
        exportStop.progress = [&](qint64 count, qint64) { processed = static_cast<int>(count); };
        exportStop.cancelled = [&] { return processed > 2; };
        QBuffer out;
        out.open(QIODevice::WriteOnly);
        QCOMPARE(SQLiteMemoryStore::exportArchiveSnapshot(
                     source.databasePath(), out, MemoryScope::Project, own(), {}, exportStop)
                     .status,
                 QString("Cancelled"));
        FullDisk disk;
        QCOMPARE(SQLiteMemoryStore::exportArchiveSnapshot(source.databasePath(), disk,
                                                          MemoryScope::Project, own(), {}, {})
                     .status,
                 QString("WriteFailure"));
        MemoryArchiveLimits limits;
        limits.maximumRecords = 3;
        QBuffer bound;
        bound.open(QIODevice::WriteOnly);
        QCOMPARE(SQLiteMemoryStore::exportArchiveSnapshot(source.databasePath(), bound,
                                                          MemoryScope::Project, own(), limits, {})
                     .status,
                 QString("LimitExceeded"));
        const auto stopped = restore(target, bytes, false, {{}, {}, [] { return false; }});
        QCOMPARE(stopped.status, QString("Cancelled"));
        QCOMPARE(target.listManagedMemory({MemoryScope::Project}, own()).records.size(), 0);
    }
    void versionOneAndHistoryFoundation() {
        QTemporaryDir dir;
        SQLiteMemoryStore source(dir.filePath("source.db")), target(dir.filePath("target.db"));
        auto record = seed(source, "history");
        const auto v1 = BackupService::exportScopedMemory(source, MemoryScope::Project, own());
        QVERIFY(v1.succeeded());
        QVERIFY(restore(target, v1.data).ok());
        for (int i = 0; i < 70; ++i) {
            MemoryMutation change;
            change.kind = MemoryMutation::Kind::Correct;
            change.record = record;
            change.record.value = QString("edit%1").arg(i);
            change.expectedRevision = record.revision;
            auto result = source.mutateManagedMemory({change}, own());
            QCOMPARE(result.status, MemoryManagementStatus::Ready);
            record = result.records.first();
        }
        QCOMPARE(record.corrections.size(), 70);
        QVERIFY(
            !BackupService::exportScopedMemory(source, MemoryScope::Project, own()).succeeded());
        SQLiteMemoryStore historyTarget(dir.filePath("history.db"));
        QVERIFY(restore(historyTarget, archive(source)).ok());
        const auto imported = historyTarget.readManagedMemory(record.id, own()).records.first();
        QCOMPARE(imported.corrections.size(), 71);
        QCOMPARE(imported.corrections.first(), record.corrections.first());
        QVERIFY(isImportedMemory(imported));
        QCOMPARE(imported.createdAtUtc, record.createdAtUtc);
        QVERIFY(imported.revision != record.revision);
        auto broken = record;
        auto receipt = broken.corrections.first().toObject();
        receipt["previousDigest"] = "broken";
        broken.corrections[0] = receipt;
        auto metadata = managedMemoryJson(broken);
        metadata.remove("revision");
        ScopedMemoryRecord parsed;
        QVERIFY(!parseManagedMemory(metadata, &parsed));
        auto full = record;
        while (full.corrections.size() < memoryHistoryLimit) {
            full.corrections.append(record.corrections.last());
        }
        QByteArray output;
        QBuffer buffer(&output);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QCOMPARE(
            writeMemoryArchive(buffer, MemoryScope::Project, own(), {}, {},
                               [&](const MemoryArchiveConsumer& consume) { return consume(full); })
                .status,
            QString("LimitExceeded"));
    }
    void interruptedRestoreRollsBack() {
        QTemporaryDir dir;
        const auto path = dir.filePath("crash.db");
        SQLiteMemoryStore source(dir.filePath("source.db"));
        for (int i = 0; i < 105; ++i) {
            seed(source, QString("crash%1").arg(i));
        }
        const auto archivePath = dir.filePath("input.smem");
        QFile file(archivePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const auto bytes = archive(source);
        QCOMPARE(file.write(bytes), qint64(bytes.size()));
        file.close();
        QProcess child;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert("P805_CRASH_FIXTURE", "1");
        child.setProcessEnvironment(environment);
        child.start(QCoreApplication::applicationFilePath(),
                    {"--crash-restore", path, archivePath});
        QVERIFY(child.waitForFinished(15000));
        QCOMPARE(child.exitCode(), 99);
        SQLiteMemoryStore recovered(path);
        QCOMPARE(recovered.listManagedMemory({MemoryScope::Project}, own()).records.size(), 0);
        QCOMPARE(recovered.memoryIndexStatus(own().access).state, MemoryIndexState::Ready);
        QVERIFY(restore(recovered, bytes).ok());
    }
    void jobPreflightCancellationAndStagingRecovery() {
        QTemporaryDir dir;
        const auto path = dir.filePath("jobs.db");
        SQLiteMemoryStore store(path);
        seed(store, "jobs");
        QString staging;
        {
            MemoryArchiveJobs jobs(path);
            auto begin =
                jobs.handle({{"operation", "archiveExportBegin"}}, MemoryScope::Project, own());
            QCOMPARE(begin.value("status").toString(), QString("Ready"));
            const auto id = begin.value("job");
            QVERIFY(!id.toString().isEmpty());
            auto poll = begin;
            QVERIFY(QTest::qWaitFor(
                [&] {
                    poll = jobs.handle({{"operation", "archivePoll"}, {"job", id}},
                                       MemoryScope::Project, own());
                    return poll.value("state") == "Exported";
                },
                5000));
            QVERIFY(poll.value("size").toInt() > 0);
            auto cancelled = jobs.handle({{"operation", "archiveCancel"}, {"job", id}},
                                         MemoryScope::Project, own());
            QCOMPARE(cancelled.value("state").toString(), QString("Cancelled"));
        }
        QDir stagingDir(dir.filePath("memory-archive-staging"));
        QFile orphan(stagingDir.filePath("transfer-orphan.part"));
        QVERIFY(orphan.open(QIODevice::WriteOnly));
        orphan.write("interrupted private spool");
        orphan.close();
        {
            MemoryArchiveJobs restarted(path);
            QVERIFY(!QFile::exists(orphan.fileName()));
        }
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("P805_CRASH_FIXTURE") == "1" && argc == 4 &&
        QString::fromLocal8Bit(argv[1]) == "--crash-restore") {
        SQLiteMemoryStore store(QString::fromLocal8Bit(argv[2]));
        QFile file(QString::fromLocal8Bit(argv[3]));
        if (!file.open(QIODevice::ReadOnly)) {
            return 98;
        }
        MemoryArchiveControl control;
        control.progress = [](qint64 records, qint64) {
            if (records > 0) {
                std::_Exit(99);
            }
        };
        store.restoreArchive(file, MemoryScope::Project, own(), {}, control, false);
        return 97;
    }
    MemoryArchiveTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_memory_archive.moc"
