// SPDX-FileCopyrightText: 2026 Sopwit <sopwith.osdev@gmail.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "sentinel/core/session/TodoService.h"
#include <QUuid>

namespace sentinel::core {

TodoService::TodoService(QObject* parent) : QObject(parent) {}
TodoService::~TodoService() = default;

void TodoService::setItems(const QList<TodoItem>& items) {
    m_items.clear();
    for (const auto& item : items) {
        m_items[item.id] = item;
    }
    emit todoUpdated();
}

QList<TodoItem> TodoService::items() const {
    return m_items.values();
}

void TodoService::updateItem(const QString& id, const QString& status, const QString& content) {
    auto it = m_items.find(id);
    if (it == m_items.end())
        return;
    it->status = status;
    if (!content.isEmpty())
        it->content = content;
    emit todoUpdated();
}

void TodoService::addItem(const QString& content, const QString& priority) {
    TodoItem item;
    do {
        item.id = generateId();
    } while (m_items.contains(item.id));
    item.content = content;
    item.priority = priority;
    item.status = "pending";
    m_items[item.id] = item;
    emit todoUpdated();
}

void TodoService::removeItem(const QString& id) {
    m_items.remove(id);
    emit todoUpdated();
}

TodoSnapshot TodoService::snapshot() const {
    return {m_items.values()};
}

void TodoService::restoreSnapshot(const TodoSnapshot& snapshot) {
    m_items.clear();
    for (const auto& item : snapshot.items) {
        m_items[item.id] = item;
    }
    emit todoUpdated();
}

QString TodoService::formattedText() const {
    QString result;
    for (const auto& item : m_items) {
        QString checkbox = item.status == "completed" ? "[x]" : "[ ]";
        result +=
            QStringLiteral("%1 %2 (priority: %3)\n").arg(checkbox, item.content, item.priority);
    }
    return result;
}

QString TodoService::generateId() const {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

} // namespace sentinel::core
