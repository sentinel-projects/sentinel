// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/session/TodoService.h"

#include <QSet>
#include <QtTest>
#include <algorithm>

using namespace sentinel::core;

class TodoServiceTest final : public QObject {
    Q_OBJECT
private slots:
    void addingAfterRestorePreservesItems_data();
    void addingAfterRestorePreservesItems();
    void snapshotRoundTripThenAdd();
};

void TodoServiceTest::addingAfterRestorePreservesItems_data() {
    QTest::addColumn<bool>("useSnapshot");
    QTest::newRow("set-items") << false;
    QTest::newRow("restore-snapshot") << true;
}

void TodoServiceTest::addingAfterRestorePreservesItems() {
    QFETCH(bool, useSnapshot);
    const QList<TodoItem> original{
        {QStringLiteral("1"), QStringLiteral("First"), QStringLiteral("completed"),
         QStringLiteral("high")},
        {QStringLiteral("2"), QStringLiteral("Second")},
        {QStringLiteral("100"), QStringLiteral("Sparse numeric ID")},
        {QStringLiteral("2147483647"), QStringLiteral("Largest signed int")},
        {QStringLiteral("custom-id"), QStringLiteral("Custom ID")},
    };
    TodoService service;
    if (useSnapshot)
        service.restoreSnapshot({original});
    else
        service.setItems(original);

    for (int i = 0; i < 5; ++i)
        service.addItem(QStringLiteral("New %1").arg(i));

    const auto items = service.items();
    QCOMPARE(items.size(), original.size() + 5);
    QSet<QString> ids;
    for (const auto& item : items) {
        QVERIFY(!item.id.isEmpty());
        QVERIFY(!ids.contains(item.id));
        ids.insert(item.id);
    }
    for (const auto& expected : original) {
        const auto found = std::find_if(items.cbegin(), items.cend(),
                                        [&](const auto& item) { return item.id == expected.id; });
        QVERIFY(found != items.cend());
        QCOMPARE(found->content, expected.content);
        QCOMPARE(found->status, expected.status);
        QCOMPARE(found->priority, expected.priority);
    }
}

void TodoServiceTest::snapshotRoundTripThenAdd() {
    TodoService original;
    original.addItem(QStringLiteral("Keep this item"));
    const auto saved = original.snapshot();
    TodoService restored;
    restored.restoreSnapshot(saved);
    restored.addItem(QStringLiteral("Add this item"));
    QCOMPARE(restored.items().size(), 2);
    const auto items = restored.items();
    QVERIFY(std::any_of(items.cbegin(), items.cend(), [&](const auto& item) {
        return item.id == saved.items.first().id && item.content == saved.items.first().content;
    }));
}

QTEST_GUILESS_MAIN(TodoServiceTest)
#include "test_todo_service.moc"
