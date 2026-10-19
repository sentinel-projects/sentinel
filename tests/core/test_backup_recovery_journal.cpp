// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/BackupRecoveryJournal.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>
#include <cstdlib>
#if defined(Q_OS_UNIX)
#include <QScopeGuard>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <cstring>
using namespace sentinel::core;
namespace {
QString id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}
QByteArray sum(const QByteArray& data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}
QString canonical(const QTemporaryDir& dir) {
    return QFileInfo(dir.path()).canonicalFilePath();
}
QByteArray beforeMemory(150000, 'm'), beforeChat(100000, 'c');
QByteArray afterMemory("restored memory"), afterChat("restored chat");
bool edit(const QString& profile, const QString& sql) {
    const auto name = id();
    bool ok = false;
    {
        auto db = QSqlDatabase::addDatabase("QSQLITE", name);
        db.setDatabaseName(QDir(profile).filePath("backup-recovery/journal.sqlite3"));
        if (db.open()) {
            QSqlQuery q(db);
            ok = q.exec(sql);
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(name);
    return ok;
}
class InterruptedRead final : public QIODevice {
public:
    InterruptedRead() {
        open(QIODevice::ReadOnly);
    }
    bool isSequential() const override {
        return false;
    }
    bool atEnd() const override {
        return false;
    }

protected:
    qint64 readData(char* data, qint64 length) override {
        if (calls_++ > 0)
            std::_Exit(77);
        const auto n = qMin(length, qint64(65536));
        std::memset(data, 'x', static_cast<size_t>(n));
        return n;
    }
    qint64 writeData(const char*, qint64) override {
        return -1;
    }

private:
    int calls_ = 0;
};
int crashChild(const QStringList& args) {
    if (args.size() != 5)
        return 90;
    BackupRecoveryJournal journal(args.at(2), args.at(3));
    if (journal.open() != "Ready" || journal.begin(id(), sum("archive")) != "Ready")
        return 91;
    const auto phase = args.at(4);
    if (phase == "Preflight")
        std::_Exit(77);
    if (journal.validated() != "Ready")
        return 92;
    if (phase == "Validated")
        std::_Exit(77);
    QBuffer memory(&beforeMemory), chat(&beforeChat);
    memory.open(QIODevice::ReadOnly);
    chat.open(QIODevice::ReadOnly);
    InterruptedRead interrupted;
    if (phase == "DuringPrepare") {
        journal.prepare({{BackupRecoveryDomain::Memory, 7, &interrupted, sum(afterMemory)}});
        return 93;
    }
    if (journal.prepare({{BackupRecoveryDomain::Memory, 7, &memory, sum(afterMemory)},
                         {BackupRecoveryDomain::Conversations, 6, &chat, sum(afterChat)}}) !=
        "Ready")
        return 94;
    if (phase == "Prepared")
        std::_Exit(77);
    if (journal.applying() != "Ready")
        return 95;
    if (phase == "Applying")
        std::_Exit(77);
    QBuffer restoredMemory(&afterMemory), restoredChat(&afterChat);
    restoredMemory.open(QIODevice::ReadOnly);
    restoredChat.open(QIODevice::ReadOnly);
    if (journal.finish(false, {{BackupRecoveryDomain::Memory, &restoredMemory},
                               {BackupRecoveryDomain::Conversations, &restoredChat}}) != "Ready")
        return 96;
    std::_Exit(77);
}
} // namespace
class BackupRecoveryJournalTest final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
#ifndef Q_OS_UNIX
        QSKIP("Journal file identity/directory durability backend is not yet supported on this "
              "platform.");
#endif
    }
    void creationIsPrivateEvenWithPermissiveUmask() {
#if defined(Q_OS_UNIX)
        QTemporaryDir dir;
        const auto profile = canonical(dir);
        const auto previousMask = ::umask(0);
        auto restoreMask = qScopeGuard([&] { ::umask(previousMask); });
        BackupRecoveryJournal journal(profile, id());
        QCOMPARE(journal.open(), QString("Ready"));
        QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        const auto root = QDir(profile).filePath("backup-recovery");
        const auto unsafe = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                            QFileDevice::ExeGroup | QFileDevice::ReadOther |
                            QFileDevice::WriteOther | QFileDevice::ExeOther;
        QVERIFY((QFile::permissions(root) & unsafe) == 0);
        QVERIFY((QFile::permissions(QDir(root).filePath("journal.sqlite3")) & unsafe) == 0);
        BackupRecoveryJournal relative(".", id());
        QCOMPARE(relative.open(), QString("InvalidInput"));
#endif
    }
    void productionStorePreimagesReopenWithSessionAndIndexIntact() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        const auto memoryPath = QDir(profile).filePath("memory.sqlite3");
        const auto chatPath = QDir(profile).filePath("conversations.sqlite3");
        QString session, memoryId;
        {
            SQLiteConversationStore conversations(chatPath);
            QCOMPARE(conversations.status(), ConversationStoreStatus::Ready);
            session = conversations.createConversation("Journal fixture").id;
            QVERIFY(!session.isEmpty());
            ConversationMessageRecord message;
            message.conversationId = session;
            message.messageId = 1;
            message.role = ChatRole::User;
            message.content = "Retain session identity";
            message.timestampUtc = QDateTime::currentDateTimeUtc();
            message.status = ChatMessageStatus::Completed;
            QVERIFY(conversations.appendMessage(message));
            SQLiteMemoryStore memory(memoryPath);
            QVERIFY(memory.isAvailable());
            MemoryAccessContext access{session, "workspace", {"workspace"}, false};
            const auto result = memory.writeScopedMemory(
                {MemoryScope::Session, "quartz", "Preserve scoped provenance"}, access);
            QCOMPARE(result.status, ScopedMemoryStatus::Ready);
            memoryId = result.records.first().id;
        } // All owning Qt SQL connections are closed before offline file readers.
        QFile memory(memoryPath), chat(chatPath);
        QVERIFY(memory.open(QIODevice::ReadOnly));
        QVERIFY(chat.open(QIODevice::ReadOnly));
        const auto memoryHash = sum(memory.readAll()), chatHash = sum(chat.readAll());
        memory.seek(0);
        chat.seek(0);
        BackupRecoveryJournal journal(profile, owner);
        QCOMPARE(journal.open(), QString("Ready"));
        QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        QCOMPARE(journal.validated(), QString("Ready"));
        QCOMPARE(journal.prepare({{BackupRecoveryDomain::Memory, 7, &memory, memoryHash},
                                  {BackupRecoveryDomain::Conversations, 6, &chat, chatHash}}),
                 QString("Ready"));
        QCOMPARE(journal.applying(), QString("Ready"));
        const auto recoveredMemory = QDir(profile).filePath("recovered-memory.sqlite3");
        const auto recoveredChat = QDir(profile).filePath("recovered-conversations.sqlite3");
        {
            QFile memoryOut(recoveredMemory), chatOut(recoveredChat);
            QVERIFY(memoryOut.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QVERIFY(chatOut.open(QIODevice::WriteOnly | QIODevice::NewOnly));
            QCOMPARE(journal.readPreimage(BackupRecoveryDomain::Memory, memoryOut),
                     QString("Ready"));
            QCOMPARE(journal.readPreimage(BackupRecoveryDomain::Conversations, chatOut),
                     QString("Ready"));
        }
        QFile memoryProof(recoveredMemory), chatProof(recoveredChat);
        QVERIFY(memoryProof.open(QIODevice::ReadOnly));
        QVERIFY(chatProof.open(QIODevice::ReadOnly));
        QCOMPARE(journal.finish(true, {{BackupRecoveryDomain::Memory, &memoryProof},
                                       {BackupRecoveryDomain::Conversations, &chatProof}}),
                 QString("Ready"));
        memoryProof.close();
        chatProof.close();
        SQLiteMemoryStore recovered(recoveredMemory);
        SQLiteConversationStore recoveredConversations(recoveredChat);
        QVERIFY(recovered.isAvailable());
        MemoryAccessContext access{session, "workspace", {"workspace"}, false};
        QCOMPARE(recovered.memoryIndexStatus(access).state, MemoryIndexState::Ready);
        const auto found = recovered.retrieveMemory("quartz", access);
        QCOMPARE(found.status, ScopedMemoryStatus::Ready);
        QCOMPARE(found.records.size(), 1);
        QCOMPARE(found.records.first().id, memoryId);
        QCOMPARE(found.records.first().ownerId, session);
        QCOMPARE(recoveredConversations.listConversations().first().id, session);
        QCOMPARE(recoveredConversations.loadMessages(session).first().content,
                 QString("Retain session identity"));
        access.sessionId = "different-session";
        QVERIFY(recovered.retrieveMemory("quartz", access).records.isEmpty());
    }
    void twoDomainsRequireIndependentVerification() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        BackupRecoveryJournal journal(profile, owner);
        QCOMPARE(journal.open(), QString("Ready"));
        QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        QCOMPARE(journal.applying(), QString("InvalidState"));
        QCOMPARE(journal.validated(), QString("Ready"));
        QBuffer memory(&beforeMemory), chat(&beforeChat);
        memory.open(QIODevice::ReadOnly);
        chat.open(QIODevice::ReadOnly);
        QCOMPARE(journal.prepare({{BackupRecoveryDomain::Memory, 7, &memory, sum(afterMemory)},
                                  {BackupRecoveryDomain::Conversations, 6, &chat, sum(afterChat)}}),
                 QString("Ready"));
        QCOMPARE(journal.inspect().value("state").toString(), QString("Prepared"));
        QVERIFY(journal.inspect().value("recoveryRequired").toBool());
        QCOMPARE(journal.applying(), QString("Ready"));
        QBuffer restoredMemory(&afterMemory), wrongChat(&beforeChat);
        restoredMemory.open(QIODevice::ReadOnly);
        wrongChat.open(QIODevice::ReadOnly);
        QCOMPARE(journal.finish(false, {{BackupRecoveryDomain::Memory, &restoredMemory},
                                        {BackupRecoveryDomain::Conversations, &wrongChat}}),
                 QString("IntegrityFailure"));
        QCOMPARE(journal.inspect().value("state").toString(), QString("Applying"));
        QCOMPARE(journal.recoveryRequired(), QString("Ready"));
        memory.seek(0);
        chat.seek(0);
        QCOMPARE(journal.finish(true, {{BackupRecoveryDomain::Memory, &memory},
                                       {BackupRecoveryDomain::Conversations, &chat}}),
                 QString("Ready"));
        QCOMPARE(journal.inspect().value("state").toString(), QString("RolledBack"));
        QVERIFY(!journal.inspect().value("recoveryRequired").toBool());
        QCOMPARE(journal.begin(id(), sum("another")), QString("InvalidInput"));
        QBuffer copied;
        copied.open(QIODevice::WriteOnly);
        QCOMPARE(journal.readPreimage(BackupRecoveryDomain::Memory, copied), QString("Ready"));
        QCOMPARE(copied.data(), beforeMemory);
        QVERIFY(!QJsonDocument(journal.inspect()).toJson().contains(profile.toUtf8()));
        QVERIFY(!QJsonDocument(journal.inspect()).toJson().contains(beforeMemory.left(100)));
    }
    void ownerLockAndProfileMismatch() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        {
            BackupRecoveryJournal first(profile, owner);
            QCOMPARE(first.open(), QString("Ready"));
            QCOMPARE(first.begin(id(), sum("archive")), QString("Ready"));
            BackupRecoveryJournal second(profile, owner);
            QCOMPARE(second.open(), QString("Busy"));
        }
        BackupRecoveryJournal wrong(profile, id());
        QCOMPARE(wrong.open(), QString("InvalidJournal"));
        QCOMPARE(wrong.begin(id(), sum("archive")), QString("InvalidInput"));
    }
    void corruptPreimageRefusesApplyingAndPublication() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        BackupRecoveryJournal journal(profile, owner);
        QCOMPARE(journal.open(), QString("Ready"));
        QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        QCOMPARE(journal.validated(), QString("Ready"));
        QBuffer memory(&beforeMemory);
        memory.open(QIODevice::ReadOnly);
        QCOMPARE(journal.prepare({{BackupRecoveryDomain::Memory, 7, &memory, sum(afterMemory)}}),
                 QString("Ready"));
        QVERIFY(edit(profile, "UPDATE preimages SET payload=x'00' WHERE domain=1 AND chunk=0"));
        QCOMPARE(journal.applying(), QString("IntegrityFailure"));
        QBuffer out;
        out.open(QIODevice::WriteOnly);
        QCOMPARE(journal.readPreimage(BackupRecoveryDomain::Memory, out),
                 QString("IntegrityFailure"));
        QVERIFY(out.data().isEmpty());
    }
    void cancelledPrepareAndUnsupportedDomainPreserveValidatedState() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        BackupRecoveryJournal journal(profile, owner);
        QCOMPARE(journal.open(), QString("Ready"));
        QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        QCOMPARE(journal.validated(), QString("Ready"));
        QBuffer memory(&beforeMemory);
        memory.open(QIODevice::ReadOnly);
        int calls = 0;
        QCOMPARE(journal.prepare({{BackupRecoveryDomain::Memory, 7, &memory, sum(afterMemory)}},
                                 [&] { return ++calls > 1; }),
                 QString("Cancelled"));
        QCOMPARE(journal.inspect().value("state").toString(), QString("Validated"));
        memory.seek(0);
        QCOMPARE(journal.prepare({{BackupRecoveryDomain::Memory, 8, &memory, sum(afterMemory)}}),
                 QString("InvalidInput"));
        QCOMPARE(journal.inspect().value("state").toString(), QString("Validated"));
        QCOMPARE(journal.finish(true, {}), QString("Ready"));
        QCOMPARE(journal.inspect().value("state").toString(), QString("RolledBack"));
    }
    void unsafeRootAndPermissionsRefused() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        const auto root = QDir(profile).filePath("backup-recovery");
        QVERIFY(QFile::link(profile, root));
        {
            BackupRecoveryJournal journal(profile, owner);
            QCOMPARE(journal.open(), QString("InvalidJournal"));
        }
        QVERIFY(QFile::remove(root));
        {
            BackupRecoveryJournal journal(profile, owner);
            QCOMPARE(journal.open(), QString("Ready"));
            QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        }
        const auto path = QDir(root).filePath("journal.sqlite3");
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ReadOther));
        {
            BackupRecoveryJournal journal(profile, owner);
            QCOMPARE(journal.open(), QString("InvalidJournal"));
        }
        QVERIFY(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
#if defined(Q_OS_UNIX)
        const auto alias = QDir(profile).filePath("hardlink-alias");
        QCOMPARE(::link(QFile::encodeName(path).constData(), QFile::encodeName(alias).constData()),
                 0);
        {
            BackupRecoveryJournal journal(profile, owner);
            QCOMPARE(journal.open(), QString("InvalidJournal"));
        }
        QVERIFY(QFile::remove(alias));
#endif
    }
    void malformedAndUnsupportedJournals_data() {
        QTest::addColumn<QString>("sql");
        QTest::newRow("future-version") << QString("PRAGMA user_version=2");
        QTest::newRow("wrong-file-identity") << QString("PRAGMA application_id=0");
        QTest::newRow("corrupt-metadata") << QString("UPDATE recovery SET payload=x'00'");
        QTest::newRow("removed-row") << QString("DELETE FROM recovery");
        QTest::newRow("unexpected-trigger") << QString(
            "CREATE TRIGGER bad AFTER UPDATE ON recovery BEGIN DELETE FROM preimages; END");
    }
    void malformedAndUnsupportedJournals() {
        QFETCH(QString, sql);
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        {
            BackupRecoveryJournal journal(profile, owner);
            QCOMPARE(journal.open(), QString("Ready"));
            QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        }
        QVERIFY(edit(profile, sql));
        BackupRecoveryJournal reopened(profile, owner);
        QCOMPARE(reopened.open(), QString("InvalidJournal"));
        QVERIFY(reopened.inspect().value("recoveryRequired").toBool());
    }
    void unsafePathsAndFileReplacement() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        BackupRecoveryJournal journal(profile, owner);
        QCOMPARE(journal.open(), QString("Ready"));
        QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        const auto path = QDir(profile).filePath("backup-recovery/journal.sqlite3");
        QVERIFY(QFile::rename(path, path + ".old"));
        QVERIFY(QFile::copy(path + ".old", path));
        QCOMPARE(journal.inspect().value("status").toString(), QString("InvalidJournal"));
        QVERIFY(journal.inspect().value("recoveryRequired").toBool());
    }
    void diskWriteFailureKeepsPreviousReceipt() {
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        BackupRecoveryJournal journal(profile, owner);
        QCOMPARE(journal.open(), QString("Ready"));
        QCOMPARE(journal.begin(id(), sum("archive")), QString("Ready"));
        const auto root = QDir(profile).filePath("backup-recovery");
        QVERIFY(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        const auto status = journal.validated();
        QVERIFY(QFile::setPermissions(root, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                                QFileDevice::ExeOwner));
        QCOMPARE(status, QString("WriteFailure"));
        QCOMPARE(journal.inspect().value("state").toString(), QString("Preflight"));
    }
    void processInterruption_data() {
        QTest::addColumn<QString>("phase");
        QTest::addColumn<QString>("expected");
        for (const auto& phase : {"Preflight", "Validated", "Prepared", "Applying", "Committed"})
            QTest::newRow(phase) << QString(phase) << QString(phase);
        QTest::newRow("uncommitted-preimages") << QString("DuringPrepare") << QString("Validated");
    }
    void processInterruption() {
        QFETCH(QString, phase);
        QFETCH(QString, expected);
        QTemporaryDir dir;
        const auto profile = canonical(dir), owner = id();
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(),
                    {"--journal-crash", profile, owner, phase});
        QVERIFY(child.waitForStarted());
        QVERIFY(child.waitForFinished(10000));
        QCOMPARE(child.exitCode(), 77);
        BackupRecoveryJournal recovered(profile, owner);
        QCOMPARE(recovered.open(), QString("Ready"));
        QCOMPARE(recovered.inspect().value("state").toString(), expected);
        QVERIFY(!recovered.inspect().value("automaticReplay").toBool());
        if (expected == "Applying" || expected == "Prepared") {
            QVERIFY(recovered.inspect().value("recoveryRequired").toBool());
            QBuffer out;
            out.open(QIODevice::WriteOnly);
            QCOMPARE(recovered.readPreimage(BackupRecoveryDomain::Memory, out), QString("Ready"));
            QCOMPARE(out.data(), beforeMemory);
            QBuffer memory(&beforeMemory), chat(&beforeChat);
            memory.open(QIODevice::ReadOnly);
            chat.open(QIODevice::ReadOnly);
            QCOMPARE(recovered.finish(true, {{BackupRecoveryDomain::Memory, &memory},
                                             {BackupRecoveryDomain::Conversations, &chat}}),
                     QString("Ready"));
            QCOMPARE(recovered.inspect().value("state").toString(), QString("RolledBack"));
        }
    }
};
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().value(1) == "--journal-crash")
        return crashChild(app.arguments());
    BackupRecoveryJournalTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "test_backup_recovery_journal.moc"
