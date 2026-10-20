// SPDX-License-Identifier: GPL-3.0-or-later
#include "sentinel/core/app/StorageAdmission.h"
#include "sentinel/core/chat/SQLiteConversationStore.h"
#include "sentinel/core/memory/SQLiteMemoryStore.h"
#include <QBuffer>
#include <QTemporaryDir>
#include <QtTest>
#include <future>
using namespace sentinel::core;
using namespace std::chrono_literals;
class StorageAdmissionTest final : public QObject {
    Q_OBJECT
private slots:
    void writersDrainAndExcludeNewWriters() {
        StorageAdmission admission;
        auto first = admission.admit(1);
        auto second = admission.admit(1);
        QVERIFY(first && second);
        auto wait = std::async(std::launch::async, [&] { return admission.prepareLocal(2s); });
        QTRY_COMPARE(admission.snapshot().state, StorageAdmission::State::Quiescing);
        QVERIFY(!admission.admit(1));
        first = {};
        QCOMPARE(admission.snapshot().writers, 1u);
        second = {};
        QCOMPARE(wait.get(), StorageAdmission::Result::Ready);
        QVERIFY(!admission.admit(1));
        QCOMPARE(admission.enterMaintenance(), StorageAdmission::Result::Ready);
        QCOMPARE(admission.enterMaintenance(), StorageAdmission::Result::Busy);
        QCOMPARE(admission.resume(), StorageAdmission::Result::Ready);
        QVERIFY(!admission.admit(1));
        QVERIFY(admission.admit(2));
    }
    void timeoutAndCancellationPreserveGeneration() {
        StorageAdmission admission;
        auto writer = admission.admit(1);
        QCOMPARE(admission.prepareLocal(0ms), StorageAdmission::Result::TimedOut);
        QCOMPARE(admission.snapshot().generation, 1u);
        auto wait = std::async(std::launch::async, [&] { return admission.prepareLocal(2s); });
        QTRY_COMPARE(admission.snapshot().state, StorageAdmission::State::Quiescing);
        QCOMPARE(admission.prepareLocal(0ms), StorageAdmission::Result::Busy);
        admission.cancel();
        QCOMPARE(wait.get(), StorageAdmission::Result::Cancelled);
        QCOMPARE(admission.snapshot().state, StorageAdmission::State::Running);
        writer = {};
        QCOMPARE(admission.prepareLocal(0ms), StorageAdmission::Result::Ready);
        QCOMPARE(admission.resume(), StorageAdmission::Result::Ready);
        QVERIFY(admission.admit(1));
    }
    void shutdownInterruptsWaitAndSurvivesLease() {
        auto admission = std::make_unique<StorageAdmission>();
        auto writer = admission->admit(1);
        auto wait = std::async(std::launch::async, [&] { return admission->prepareLocal(2s); });
        QTRY_COMPARE(admission->snapshot().state, StorageAdmission::State::Quiescing);
        admission->shutdown();
        QCOMPARE(wait.get(), StorageAdmission::Result::Unavailable);
        QCOMPARE(admission->resume(), StorageAdmission::Result::Unavailable);
        QVERIFY(!admission->admit(1));
        admission.reset();
        writer = {};
    }
    void realMemoryWritesIndexAndArchiveAreFenced() {
        QTemporaryDir profile;
        QVERIFY(profile.isValid());
        auto admission = std::make_shared<StorageAdmission>();
        SQLiteMemoryStore store(profile.filePath("memory.sqlite"), admission);
        QVERIFY(store.isAvailable());
        MemoryAccessContext context;
        context.sessionId = "session";
        ScopedMemoryWrite write;
        write.key = "key";
        write.value = "value";
        QCOMPARE(store.writeScopedMemory(write, context).status, ScopedMemoryStatus::Ready);
        store.put("legacy", "before");
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        store.put("legacy", "after");
        store.clear();
        QCOMPARE(store.get("legacy"), QString("before"));
        QCOMPARE(store.writeScopedMemory(write, context).status, ScopedMemoryStatus::Unavailable);
        QCOMPARE(store.refreshMemoryIndex(context).state, MemoryIndexState::Unavailable);
        MemoryManagementContext management{context, true};
        QCOMPARE(store.clearManagedMemory(MemoryScope::Session, management).status,
                 MemoryManagementStatus::Unavailable);
        QCOMPARE(store.mutateManagedMemory({MemoryMutation{}}, management).status,
                 MemoryManagementStatus::Unavailable);
        QBuffer archive;
        archive.open(QIODevice::ReadOnly);
        QVERIFY(
            !store.restoreArchive(archive, MemoryScope::Session, management, {}, {}, false).ok());
        // Even opening a second writer connection/schema must be refused while quiescent.
        SQLiteMemoryStore worker(profile.filePath("memory.sqlite"), admission, 1);
        QVERIFY(!worker.isAvailable());
        QCOMPARE(admission->enterMaintenance(), StorageAdmission::Result::Ready);
        QCOMPARE(admission->resume(), StorageAdmission::Result::Ready);
        store.put("legacy", "stale");
        QCOMPARE(store.get("legacy"), QString("before"));
        SQLiteMemoryStore staleWorker(profile.filePath("memory.sqlite"), admission, 1);
        QVERIFY(!staleWorker.isAvailable());
        SQLiteMemoryStore replacement(profile.filePath("memory.sqlite"), admission, 2);
        QVERIFY(replacement.isAvailable());
        replacement.put("legacy", "new");
        QCOMPARE(replacement.get("legacy"), QString("new"));
    }
    void conversationCompletionAndTitleCannotRegainAuthority() {
        QTemporaryDir profile;
        auto admission = std::make_shared<StorageAdmission>();
        SQLiteConversationStore store(profile.filePath("conversations.sqlite"), admission);
        auto conversation = store.createConversation("New Chat");
        QVERIFY(!conversation.id.isEmpty());
        ConversationMessageRecord message;
        message.conversationId = conversation.id;
        message.messageId = 1;
        message.role = ChatRole::Assistant;
        message.content = "placeholder";
        message.timestampUtc = QDateTime::currentDateTimeUtc();
        message.status = ChatMessageStatus::Streaming;
        QVERIFY(store.appendMessage(message));
        auto titleCallback = [&] {
            return store.autoTitleConversation(conversation.id, "late title");
        };
        auto completion = [&] {
            message.status = ChatMessageStatus::Completed;
            return store.appendMessage(message);
        };
        QCOMPARE(admission->prepareLocal(0ms), StorageAdmission::Result::Ready);
        QVERIFY(!titleCallback());
        QVERIFY(!completion());
        QVERIFY(!store.renameConversation(conversation.id, "rename"));
        QVERIFY(!store.pinConversation(conversation.id));
        QVERIFY(!store.archiveConversation(conversation.id));
        QVERIFY(!store.deleteConversation(conversation.id));
        QVERIFY(!store.clearHistory());
        QCOMPARE(admission->enterMaintenance(), StorageAdmission::Result::Ready);
        QCOMPARE(admission->resume(), StorageAdmission::Result::Ready);
        QVERIFY(!titleCallback());
        QVERIFY(!completion());
        QCOMPARE(store.loadMessages(conversation.id).first().status, ChatMessageStatus::Streaming);
        QCOMPARE(store.listConversations().first().title, QString("New Chat"));
    }
    void uncoveredOwnersAreNeverApplicationQuiescence() {
        StorageAdmission live(StorageAdmission::Scope::LiveApplication);
        QCOMPARE(live.prepareLocal(0ms), StorageAdmission::Result::Unsupported);
        QCOMPARE(live.snapshot().state, StorageAdmission::State::Running);
        StorageAdmission admission;
        QCOMPARE(admission.prepareApplication(), StorageAdmission::Result::Unsupported);
        QCOMPARE(admission.prepareLocal(0ms), StorageAdmission::Result::Ready);
        QCOMPARE(admission.prepareApplication(), StorageAdmission::Result::Unsupported);
        QCOMPARE(admission.resume(), StorageAdmission::Result::Ready);
    }
};
QTEST_GUILESS_MAIN(StorageAdmissionTest)
#include "test_storage_admission.moc"
