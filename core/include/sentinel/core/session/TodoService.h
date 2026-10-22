// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sentinel/core/session/ITodoService.h"
#include <QMap>
#include <QObject>

namespace sentinel::core {

class TodoService : public QObject, public ITodoService {
    Q_OBJECT
public:
    explicit TodoService(QObject* parent = nullptr);
    ~TodoService() override;

    void setItems(const QList<TodoItem>& items) override;
    QList<TodoItem> items() const override;
    void updateItem(const QString& id, const QString& status, const QString& content = {}) override;
    void addItem(const QString& content, const QString& priority = "medium") override;
    void removeItem(const QString& id) override;
    TodoSnapshot snapshot() const override;
    void restoreSnapshot(const TodoSnapshot& snapshot) override;
    QString formattedText() const override;

signals:
    void todoUpdated();

private:
    QString generateId() const;

    QMap<QString, TodoItem> m_items;
};

} // namespace sentinel::core
